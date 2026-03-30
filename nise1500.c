//  MZ-1500 Emulator for MZ-700
//
//  MZ-1R12: RAM Memory
//  MZ-1R18: RAMFILE
//  MZ-1R23: Kanji ROM
//  MZ-1R24: Jisho ROM
//  PIO-3034: EMM

//  MZ-700 extention slot
//  GP0-7:  D0-7
//  GP8-23: A0-15
//  GP24: EXINT
//  GP25: RESET -> Interrupt
//  GP26: M1
//  GP28: MERQ
//  GP29: IORQ
//  GP30: WR
//  GP31: RD
//  GP32: EXWAIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/pwm.h"

#include "vga16_graphics.h"

#include "misc.h"
#include "lfs.h"

#define RESET_PIN 25

// RAM configuration
// MZ-1R18: 64KiB
// MZ-1R12: 32KiB
// EMM:    320KiB

uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) mz1r12[0x8000];
uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) mz1r18[0x10000];
uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) emm[0x50000];

// PCG emulation (36KiB)
// VRAM  4KiB
// CGRAM 4Kib
// PCG   24KiB

uint8_t vram[0x1000];
uint8_t cgram[0x1000];
uint8_t pcg[0x2000 * 3];
uint8_t pcg700[0x800];
uint8_t memioport[0x10];

volatile uint8_t vram_enabled;
volatile uint8_t pcg_enabled;

uint8_t extram[0x1800];

//#define PCGWAIT             // Enable WAIT for PCG-RAM write access
#define PSGWAIT             // Enable WAIT for PSG write

#define MAXROMPAGE 64       // =  32KiB * 64 pages = 2048KiB
#define MAXEMMPAGE 16       // = 320KiB * 16 pages = 5120KiB

volatile uint8_t rompage=0;  // Page No of MZ-1R12 (64KiB/page)
volatile uint8_t emmpage=0;  // Page No of EMM (320KiB/page)

volatile uint32_t kanjiptr=0;
volatile uint32_t dictptr=0;
volatile uint32_t kanjictl=0;
volatile uint32_t mz1r18ptr=0;
volatile uint32_t mz1r12ptr=0;
volatile uint32_t emmptr=0;
volatile uint32_t flash_command=0;
volatile uint32_t pcg700_ptr=0;

// ROM configuration
// WeACT RP2350B with 16MiB Flash
// EXT ROM          8KiB   @ 0x1001d000
// FONT ROM         4KiB   @ 0x1001f000
// Kanji ROM        128KiB @ 0x10020000
// Jisho ROM        256KiB @ 0x10040000
// Data for 1R12    32KiB  * 64 = 2048KiB   @ 0x10080000
// Data for EMM     320KiB * 16 = 5120KiB   @ 0x10280000
// LittleFS         8MiB  @ 0x10800000

#define ROMBASE 0x10080000u

uint8_t *extrom=(uint8_t *)(ROMBASE-0x63000);
uint8_t *fontrom=(uint8_t *)(ROMBASE-0x61000);
uint8_t *kanjirom=(uint8_t *)(ROMBASE-0x60000);
uint8_t *jishorom=(uint8_t *)(ROMBASE-0x40000);
uint8_t *romslots=(uint8_t *)(ROMBASE);
uint8_t *emmslots=(uint8_t *)(ROMBASE+(0x8000*MAXROMPAGE));

// Define the flash sizes
// This is setup to read a block of the flash from the end 
#define BLOCK_SIZE_BYTES (FLASH_SECTOR_SIZE)
#define HW_FLASH_STORAGE_BYTES  (8 * 1024 * 1024)
#define HW_FLASH_STORAGE_BASE   (PICO_FLASH_SIZE_BYTES - HW_FLASH_STORAGE_BYTES) 

lfs_t lfs;
lfs_file_t lfs_file;

// VGA Output
uint8_t scandata[320];
extern unsigned char vga_data_array[];
volatile uint32_t video_hsync,video_vsync,scanline,vsync_scanline;
volatile uint8_t pcg_control;
volatile uint8_t pcg700_control,pcg700_data;
uint8_t pallet[8];

uint8_t colors[16]; // DUMMY
uint16_t status[40]; // Status line (26th line)

// QD
volatile uint8_t qd_status=0;
uint8_t qd_filename[65];
lfs_file_t qd_drive;
uint8_t qd_motor=0;
volatile uint8_t qd_data;
uint32_t qd_ptr=0;
uint32_t qd_blocksize=0;
uint32_t qd_sync=0;
uint32_t qd_type=0;
uint32_t qd_stage=0;
uint8_t qd_numblocks=0;
uint32_t qd_count;
volatile uint8_t qd_data_ready=0;
volatile uint32_t qd_data_request;

uint8_t qd_header[]={0xa5,0x00,0x00,0x00,0x16};

uint32_t qdimage_numfiles;
uint32_t qdimage_selected=0;
uint32_t qdimage_display_count=0;

#define QDIMAGE_TIME   600

// BEEP & PSG

uint32_t pwm_slice_num;
volatile uint32_t sound_tick=0;

#define PSG_NUMBERS 2

uint16_t psg_register[8 * PSG_NUMBERS];
uint32_t psg_osc_interval[3 * PSG_NUMBERS];
uint32_t psg_osc_counter[3 * PSG_NUMBERS];

uint32_t psg_noise_interval[PSG_NUMBERS];
uint32_t psg_noise_counter[PSG_NUMBERS];
uint8_t psg_noise_output[PSG_NUMBERS];
uint32_t psg_noise_seed[PSG_NUMBERS];
uint32_t psg_freq_write[PSG_NUMBERS];
// uint32_t psg_envelope_interval[PSG_NUMBERS];
// uint32_t psg_envelope_counter[PSG_NUMBERS];
//uint32_t psg_master_clock = PSG_CLOCK2;
//uint32_t psg_master_clock = (3579545/2);
uint32_t psg_master_clock = 3579545;
uint16_t psg_master_volume = 0;

// TESUTO
uint32_t psg_note_count;

uint16_t psg_volume[] = { 0xFF,0xCB,0xA1,0x80,0x66,0x51,0x40,0x33,0x28,0x20,0x1A,0x14,0x10,0x0D,0x0A,0x00};

//#define SAMPLING_FREQ 48000
#define SAMPLING_FREQ 31500
//#define SAMPLING_FREQ 22050
#define TIME_UNIT 100000000                           // Oscillator calculation resolution = 10nsec
#define SAMPLING_INTERVAL (TIME_UNIT/SAMPLING_FREQ) 

#define I8253CLOCK 894000 // NTSC
//#define I8253CLOCK 110800 // PAL

volatile uint8_t sioa[8],siob[8];
volatile uint8_t sioa_access,siob_access;

volatile uint8_t pioa[4],piob[4];
volatile uint32_t pioa_enable_irq=0;
volatile uint32_t piob_enable_irq=1;
uint32_t pioa_next_mask,pioa_next_iocontrol;
uint32_t piob_next_mask,piob_next_iocontrol;
uint32_t pio_irq_processing=0;
volatile uint8_t pioa_data;
//
uint8_t pioa_int_debug=0;

volatile uint8_t i8253[3];
volatile uint8_t i8253_access[3];
volatile uint16_t i8253_preload[3];
volatile uint16_t i8253_counter[3];
volatile uint8_t i8253_pending[3];
volatile uint16_t i8253_latch[3];
volatile uint32_t i8253_latch_flag=0;
volatile uint32_t i8253_enable_irq=0;
volatile uint32_t beep_flag=0;
volatile uint32_t beep_mute=0;

// Button status

uint32_t button1=0;
uint32_t button2=0;
volatile uint32_t button1_pressed=0;
volatile uint32_t button2_pressed=0;
volatile uint32_t button1_long=0;
volatile uint32_t button2_long=0;
#define BUTTON_MIN 10
#define BUTTON_LONG 300

//uint8_t __attribute__  ((aligned(sizeof(unsigned char *)*4096))) flash_buffer[4096];

void __not_in_flash_func(mzscan)(uint8_t scan);
void __not_in_flash_func(statusscan)(uint8_t scan);
bool __not_in_flash_func(sound_handler)(void);

// *REAL* H-Sync for emulation
void __not_in_flash_func(hsync_handler)(void) {

    uint32_t vramindex;
    uint32_t tmsscan;
    uint8_t bgcolor;

    pio_interrupt_clear(pio0, 0);

        sound_handler();

    if((scanline!=0)&&(gpio_get(VSYNC)==0)) { // VSYNC
        scanline=0;
        video_vsync=1;

        // check button

        if(gpio_get(45)!=0) {
            button1++;
        } else {
            if(button1>BUTTON_LONG) {
                button1_long=1;
            } else if (button1>BUTTON_MIN) {
                button1_pressed=1;
            }
            button1=0;
        }

        if(gpio_get(46)!=0) {
           button2++;
        } else {
            if(button2>BUTTON_LONG) {
                button2_long=1;
            } else if (button2>BUTTON_MIN) {
                button2_pressed=1;
            }
            button2=0;
        }

        if(qdimage_display_count) {
            qdimage_display_count--;
            if(qdimage_display_count==0) {
                memset(status,0,80);
            }
        }

    } else {
        scanline++;
    }

    if((scanline%2)==0) {
        video_hsync=1;

        // VDP Draw on HSYNC

        // VGA Active starts scanline 35
        //          Active scanline 78(0) to 477(199)

        if((scanline>=73)&&(scanline<=(472+16))) {
//        if((scanline>=81)&&(scanline<=480)) {

            tmsscan=(scanline-73)/2;
//            tmsscan=(scanline-81)/2;
            vramindex=(tmsscan%4)*320;

            if(tmsscan<200) {
                mzscan(tmsscan);
            } else {
                statusscan(tmsscan-200);
            }



            for(int j=0;j<320;j++) {
                vga_data_array[vramindex+j]=scandata[j];
            }           
        }

    }

    // run i8253 channel 1/2
    // Channel 1 15.75kHz (mode 2)
    // Channel 2 (mode 0)

        if((i8253_access[1]<2)&&(i8253_preload[1]!=0)) {
            if(i8253_counter[1]>1) {
                i8253_counter[1]--;
                if (i8253_counter[1]==1) {
                    if((i8253_access[2]<2)&&(i8253_preload[2]!=0)){
                        if(i8253_pending[2]) {
                            i8253_pending[2]=0;
                        } else {
                            if (i8253_counter[2]>1) {
                                i8253_counter[2]--;
                            } else {
//                            i8253_counter[2]=i8253_preload[2];
                              i8253_counter[2]=65535;
                                // Timer Interrupt
                                // Check mask
                                if(memioport[2]&0x4) { 
                                    i8253_enable_irq=2;
                                }
                                if((pioa[2]&0x80)&&((pioa[3]&0x20)==0)) { 
                                    pioa_enable_irq=1;

                                    pioa_data|=0x20;
//                                    ioport[0xfe]|=0x20;
                                }
                            }
                        } 
                    }
                }
            } else {
                i8253_counter[1]=i8253_preload[1];
            }
        }

//        sound_handler();

    return;

}

// BEEP and PSG emulation
//bool __not_in_flash_func(sound_handler)(struct repeating_timer *t) {
bool __not_in_flash_func(sound_handler)(void) {

    uint16_t timer_diffs;
    uint32_t pon_count;
    uint16_t master_volume;
    uint32_t beep_on,beep_volume;
    uint8_t tone_output[3 * PSG_NUMBERS], noise_output[3 * PSG_NUMBERS];

    pwm_set_chan_level(pwm_slice_num,PWM_CHAN_B,psg_master_volume);

    // BEEP
    // i8253 timer 0

    beep_on=0;

    if(memioport[8]&1) {  // i8253 GATE#0 enable

        timer_diffs= I8253CLOCK / SAMPLING_FREQ;

        if(timer_diffs<i8253_counter[0]){
            i8253_counter[0]-=timer_diffs;

            if(memioport[2]&1) {  // Beep flag

                beep_on=1;

                if((i8253[0]&0xe)==6) {  // Mode 3

                    if(i8253_counter[0]>(i8253_preload[0]/2)) {
                        beep_volume=255;
                    } else {
                        beep_volume=0;
                    }
                } else if((i8253[0]&0xe)==0){ // mode 0                    
                    beep_volume=0;
                } else if((i8253[0]&0xe)==0xa){ // mode 5                    
                    beep_volume=255;
                }
            } else {
                beep_on=0;
            }

        } else {
            i8253_counter[0]=i8253_counter[0]+(i8253_preload[0]-timer_diffs);

            // interrupt via Z80PIO

                if((pioa[2]&0x80)&&((pioa[3]&0x10)==0)) {
                        pioa_enable_irq=1;
                        pioa_data|=0x10;
//                        ioport[0xfe]|=0x10;

                }

        }
    } else {
        if(memioport[2]&1) {
            beep_on=1; 
            beep_volume=255;
        } else {
            beep_on=0;
            beep_volume=0;
        }
    }

    // PSG

    // Run Noise generator

    for (int i = 0; i < PSG_NUMBERS; i++) {

        psg_noise_counter[i] += SAMPLING_INTERVAL;
        if (psg_noise_counter[i] > psg_noise_interval[i]) {
            psg_noise_seed[i] = (psg_noise_seed[i] >> 1)
                    | (((psg_noise_seed[i] << 14) ^ (psg_noise_seed[i] << 16))
                            & 0x10000);
            psg_noise_output[i] = psg_noise_seed[i] & 1;
            psg_noise_counter[i] -= psg_noise_interval[i];
        }

    }

    // Run Oscillator

    for (int i = 0; i < 3 * PSG_NUMBERS; i++) {
        pon_count = psg_osc_counter[i] += SAMPLING_INTERVAL;
        if (pon_count < (psg_osc_interval[i] / 2)) {
//            tone_output[i] = psg_tone_on[i];
            tone_output[i] = 1;
        } else if (pon_count > psg_osc_interval[i]) {
            psg_osc_counter[i] -= psg_osc_interval[i];
//            tone_output[i] = psg_tone_on[i];
            tone_output[i] = 1;
        } else {
            tone_output[i] = 0;
        }
    }

    // Mixer

    master_volume = 0;
    psg_note_count=0;

    for (int i = 0; i < PSG_NUMBERS; i++) {
        for (int j = 0; j < 3; j++) {
            if(tone_output[j+i*3]) {
                master_volume+=psg_volume[psg_register[j*2+i*8+1]];
//                master_volume+=psg_volume[32];
            } 
        }
        if(psg_noise_output[i]) {
            master_volume+=psg_volume[psg_register[7+i*8]];
        }
    }

    // count enable channels

    for (int i = 0; i < PSG_NUMBERS; i++) {
        for (int j = 0; j < 4; j++) {
            if(psg_register[j*2+i*8+1]!=0xf) {
                    psg_note_count++;
            }            
        }
    }

    if(beep_on) {
        psg_note_count++;
        master_volume+=beep_volume;
    }

//    psg_master_volume = master_volume / (3 * PSG_NUMBERS);
    psg_master_volume = master_volume / psg_note_count;


    return true;
}

// PSG virtual registers
// 0: CH0 Freq
// 1: CH0 Volume
// 6: Noise Freq
// 7: Noise Volume

static inline void psg_write(uint32_t psg_no,uint32_t data) {

    uint32_t channel,freqdiv,freq;

    if(data&0x80) {

        channel=(data&0x60)>>5;
        psg_freq_write[psg_no]=0;

        switch((data&0x70)>>4) {

            // Frequency

            case 0:
            case 2:
            case 4:

                psg_register[psg_no*8+channel*2]=data&0xf;
                psg_freq_write[psg_no]=channel;
                break;

            case 6:  // WIP
                psg_register[psg_no*8+6]=data&0xf;
                switch(data&3){
                    case 0:
                        freqdiv=512;
                        break;
                    case 1:
                        freqdiv=1024;
                        break;
                    case 2:
                        freqdiv=2048;
                        break;
                    case 3:
                        freqdiv=psg_register[psg_no*8+4];
                }


                if(freqdiv==0) {
                    psg_noise_interval[psg_no]=UINT32_MAX;
                    return;
                }

                freq= psg_master_clock / freqdiv;
                freq>>=5;

                if(freq==0) {
                    psg_noise_interval[psg_no]=UINT32_MAX; 
                } else {
                    psg_noise_interval[psg_no]= TIME_UNIT/freq;
                    psg_noise_counter[psg_no]=0;
                }

                break;

            // volume

            case 1:
            case 3:
            case 5:
            case 7:
            
                psg_register[psg_no*8+channel*2+1]=data&0xf;

                break;

        }

    } else {

        uint32_t noise_flag=psg_register[psg_no*8+6]&3;
        
        channel=psg_freq_write[psg_no];
        psg_register[psg_no*8+channel*2]|=(data&0x3f)<<4;

        freqdiv=psg_register[psg_no*8+channel*2];

        if(freqdiv==0) {
            psg_osc_interval[psg_no*3+channel]=UINT32_MAX;
            if(noise_flag==3) {
                psg_noise_interval[psg_no]=UINT32_MAX;
            }
            return;
        }

        freq= psg_master_clock / freqdiv;
        freq>>=5;

        if(freq==0) {
            psg_osc_interval[psg_no*3+channel]=UINT32_MAX; 
            if(noise_flag==3) {
                psg_noise_interval[psg_no]=UINT32_MAX;
            }
        } else {
            psg_osc_interval[psg_no*3+channel]= TIME_UNIT/freq;
            if(psg_osc_counter[psg_no*3+channel]>psg_osc_interval[psg_no*3+channel]) {
                psg_osc_counter[psg_no*3+channel]=0;
            }

            if(noise_flag==3) {
                psg_noise_interval[psg_no]=TIME_UNIT/freq;
                psg_noise_counter[psg_no]=0;
            }

        }

    }    
}

void psg_reset(int flag) {

    psg_noise_seed[0] = 12345;
    psg_noise_seed[1] = 12345;


    for (int i = 0; i < 16; i+=2) {
        psg_register[i] = 0;
        psg_register[i+1] = 0xf;
    }

    psg_noise_interval[0] = UINT32_MAX;
    psg_noise_interval[1] = UINT32_MAX;

    for (int i = 0; i < 6; i++) {
        psg_osc_interval[i] = UINT32_MAX;
//        psg_tone_on[i] = 0;
//        psg_noise_on[i] = 0;
    }

}

//
//  reset

void __not_in_flash_func(z80reset)(uint gpio,uint32_t event) {

//    gpio_acknowledge_irq(RESET_PIN,GPIO_IRQ_EDGE_FALL);
    gpio_acknowledge_irq(RESET_PIN,GPIO_IRQ_EDGE_RISE);

    if(gpio_get(RESET_PIN)==0) return;

    kanjiptr=0;
    dictptr=0;
    kanjictl=0;
    mz1r18ptr=0;
    mz1r12ptr=0;
    emmptr=0;
    pcg_control=0;
    pcg700_ptr=0;
    pcg700_control=0xff;

    vram_enabled=1;
    pcg_enabled=0xff;

    memset(status,0,80);

    psg_reset(0);
    psg_reset(1);

    return;
}

// VGA Out

void __not_in_flash_func(mzscan)(uint8_t scan) {

    uint8_t scanx,scany,scanyy;
    uint8_t ch,color,font,pcgfontb,pcgfontr,pcgfontg,fgcolor,bgcolor;
    uint16_t pcgch;
    uint32_t offset;
    uint8_t scan_buffer[8];

    union bytemember {
         uint32_t w;
         uint8_t b[4];
    };

    union bytemember bitf1,bitf2,bitb1,bitb2,bitp1,bitp2;

    scany=scan/8;
    scanyy=scan%8;
    offset=scany*40;

    for(scanx=0;scanx<40;scanx++) {

        // Charactor data
        ch=vram[offset];
        color=vram[offset+0x800];

        if(((pcg700_control&8)==0)&&(ch>0x7f)) {
            if(color&0x80) {
                font=pcg700[(ch&0x7f)*8+scanyy+0x400];
            } else {
                font=pcg700[(ch&0x7f)*8+scanyy];
            }
        } else {
            if(color&0x80) {
                font=cgram[ch*8+scanyy+0x800];
            } else {
                font=cgram[ch*8+scanyy];
            }
        }

        fgcolor=(color&0x70)>>4;
        bgcolor=color&7;

        bitf1.w=bitexpand[font*4  ]*fgcolor;
        bitf2.w=bitexpand[font*4+1]*fgcolor;

        bitb1.w=bitexpand[font*4+2]*bgcolor;
        bitb2.w=bitexpand[font*4+3]*bgcolor;

        // PCG data
        if((pcg_control&1) && (vram[offset+0xc00]&8)) {

            pcgch=vram[offset+0x400]+((uint16_t)vram[offset+0xc00]&0xc0)*4;

            pcgfontb=pcg[pcgch*8+scanyy];
            pcgfontr=pcg[pcgch*8+scanyy+0x2000];
            pcgfontg=pcg[pcgch*8+scanyy+0x4000];

            bitp1.w=bitexpand[pcgfontb*4  ]+bitexpand[pcgfontr*4  ]*2+bitexpand[pcgfontg*4  ]*4;
            bitp2.w=bitexpand[pcgfontb*4+1]+bitexpand[pcgfontr*4+1]*2+bitexpand[pcgfontg*4+1]*4;

        } else {
            bitp1.w=0;
            bitp2.w=0;
        }
        
        // Merge
        if(pcg_control&2) {
            // PCG > TEXT
            for(int i=0;i<4;i++) {
                if(bitp1.b[i]!=0) {
                    scan_buffer[i+4]=bitp1.b[i];
                } else {
                    scan_buffer[i+4]=bitf1.b[i]+bitb1.b[i];
                }
                if(bitp2.b[i]!=0) {
                    scan_buffer[i]=bitp2.b[i];
                } else {
                    scan_buffer[i]=bitf2.b[i]+bitb2.b[i];
                }                        
            }
        } else {
            // TEXT > PCG
            for(int i=0;i<4;i++) {
                if(bitf1.b[i]!=0) {
                    scan_buffer[i+4]=bitf1.b[i];
                } else if(bitp1.b[i]!=0) {
                    scan_buffer[i+4]=bitp1.b[i];
                } else {
                    scan_buffer[i+4]=bitb1.b[i];
                }
                if(bitf2.b[i]!=0) {
                    scan_buffer[i]=bitf2.b[i];
                } else if(bitp2.b[i]!=0) {
                    scan_buffer[i]=bitp2.b[i];
                } else {
                    scan_buffer[i]=bitb2.b[i];
                }
            }
        }

    // Color pallet

        for(int i=0;i<8;i++) {
                      scandata[scanx*8+i]=pallet[scan_buffer[i]];     
        }

        offset++;

    }

    return;

}

//  render status line

void __not_in_flash_func(statusscan)(uint8_t scan) {

    uint8_t scanx,scany,scanyy;
    uint8_t color,font,pcgfontb,pcgfontr,pcgfontg,fgcolor,bgcolor;
    uint32_t offset;
    uint32_t *scandataw;
    uint16_t ch;

    scanyy=scan%8;
    offset=0;
    scandataw=(uint32_t *)scandata;

    for(scanx=0;scanx<40;scanx++) {

        // Charactor data
        ch=status[offset];
        color=0x70;     // white

        font=cgram[ch*8+scanyy];

        fgcolor=(color&0x70)>>4;
        bgcolor=color&7;

        scandataw[scanx*2+1]=bitexpand[font*4  ]*fgcolor;
        scandataw[scanx*2  ]=bitexpand[font*4+1]*fgcolor;

        offset++;

    }

    return;

}



//// Quick Disk
// LittleFS

int pico_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t fs_start = XIP_BASE + HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (block * c->block_size) + off;
    
//    printf("[FS] READ: %p, %d\n", addr, size);
    
    memcpy(buffer, (unsigned char *)addr, size);
    return 0;
}

int pico_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t addr = fs_start + (block * c->block_size) + off;
    
//    printf("[FS] WRITE: %p, %d\n", addr, size);
        
    uint32_t ints = save_and_disable_interrupts();
//    multicore_lockout_start_blocking();     // pause another core
    flash_range_program(addr, (const uint8_t *)buffer, size);
//    multicore_lockout_end_blocking();
    restore_interrupts(ints);
    
    return 0;
}

int pico_erase(const struct lfs_config *c, lfs_block_t block)
{           
    uint32_t fs_start = HW_FLASH_STORAGE_BASE;
    uint32_t offset = fs_start + (block * c->block_size);
    
//    printf("[FS] ERASE: %p, %d\n", offset, block);
        
    uint32_t ints = save_and_disable_interrupts();   
//    multicore_lockout_start_blocking();     // pause another core
    flash_range_erase(offset, c->block_size);  
//    multicore_lockout_end_blocking();
    restore_interrupts(ints);

    return 0;
}

int pico_sync(const struct lfs_config *c)
{
    return 0;
}

// configuration of the filesystem is provided by this struct
const struct lfs_config PICO_FLASH_CFG = {
    // block device operations
    .read  = &pico_read,
    .prog  = &pico_prog,
    .erase = &pico_erase,
    .sync  = &pico_sync,

    // block device configuration
    .read_size = FLASH_PAGE_SIZE, // 256
    .prog_size = FLASH_PAGE_SIZE, // 256
    
    .block_size = BLOCK_SIZE_BYTES, // 4096
    .block_count = HW_FLASH_STORAGE_BYTES / BLOCK_SIZE_BYTES, // 352
    .block_cycles = 16, // ?
    
    .cache_size = FLASH_PAGE_SIZE, // 256
    .lookahead_size = FLASH_PAGE_SIZE,   // 256    
};

/// QD Emulation

void qd_check() {

    uint8_t qdftmp[16];

    lfs_file_seek(&lfs,&qd_drive,0,LFS_SEEK_SET);
    lfs_file_read(&lfs,&qd_drive,qdftmp,16);

    if(memcmp(qdftmp,"-QD format-",11)==0) {
        qd_type=0;
    } else {
        qd_ptr=0;
        qd_numblocks=0;
        while(qd_ptr<lfs_file_size(&lfs,&qd_drive)) {

            lfs_file_seek(&lfs,&qd_drive,qd_ptr,LFS_SEEK_SET);
            lfs_file_read(&lfs,&qd_drive,qdftmp,16);
            lfs_file_read(&lfs,&qd_drive,qdftmp,16);
            qd_blocksize=qdftmp[3]*256+qdftmp[2];

            qd_ptr+=128;
            qd_ptr+=qd_blocksize;
            qd_numblocks+=2;
        }

        qd_type=1;
        qd_stage=0;
        qd_count=0;
    }

    lfs_file_seek(&lfs,&qd_drive,0,LFS_SEEK_SET);    

}


uint8_t qd_read() {

    uint8_t qd_data,qd_mark,size_hi,size_lo;
    static uint32_t qd_blockcount;
    int32_t lfs_status;
    uint32_t qd_filesize;

    if(qd_status==0) {
        return 0xff;
    }

    if(qd_type==0) { // QDF format

        qd_filesize=lfs_file_size(&lfs,&qd_drive);

    if(qd_motor) {

        // find sync

        if(qd_sync) {

            qd_data=0;
            while(qd_data!=0x16) { // Find Sync
                lfs_status=lfs_file_read(&lfs,&qd_drive,&qd_data,1);
                if(lfs_status<0) {
                    qd_motor=0;
                    return 0xff;
                }
                qd_ptr++;
                if(qd_ptr>=qd_filesize) {
                    qd_motor=0;
                    return 0xff;
                }
            }

            while(qd_data==0x16) { // Find Sync
                lfs_status=lfs_file_read(&lfs,&qd_drive,&qd_data,1);
                if(lfs_status<0) {
                    qd_motor=0;
                    return 0xff;
                }
                qd_ptr++;
                if(qd_ptr>=qd_filesize) {
                    qd_motor=0;
                    return 0xff;
                }
            }

            qd_sync=0;
            qd_ptr--;

//            lfs_file_seek(&lfs,&qd_drive,qd_ptr,LFS_SEEK_SET);  // Not needed ?

            lfs_file_read(&lfs,&qd_drive,&qd_mark,1);

            if(qd_stage==0) {  // first record
                qd_blocksize=5;
                qd_stage=1;
            } else {
                lfs_file_read(&lfs,&qd_drive,&size_lo,1);
                lfs_file_read(&lfs,&qd_drive,&size_hi,1);
                qd_blocksize=size_lo+size_hi*256+7;
            }

            // if((qd_mark==4)||(qd_mark==2)||(qd_mark==0xff)) {
            //     qd_blocksize=5;
            // } else {
            //     lfs_file_read(&lfs,&qd_drive,&size_lo,1);
            //     lfs_file_read(&lfs,&qd_drive,&size_hi,1);
            //     qd_blocksize=size_lo+size_hi*256+7;
            // }

            lfs_file_seek(&lfs,&qd_drive,qd_ptr,LFS_SEEK_SET); 

//            printf("\n\r[QDM:%02x/%05x/%04x]",qd_data,qd_ptr,qd_blocksize);

        }

        if(qd_ptr<=qd_filesize) {

            qd_blocksize--;
            if(qd_blocksize==0) {
//                qd_ptr+=8;  // ?
                  qd_ptr+=16;  // ?
                lfs_file_seek(&lfs,&qd_drive,qd_ptr,LFS_SEEK_SET);

                qd_sync=1;
                return 0x16;
            }            

            lfs_file_read(&lfs,&qd_drive,&qd_data,1);
            qd_ptr++;
            return qd_data;

        } else {

            qd_motor=0;
            qd_data_request=0;
            qd_data_ready=0;

            return 0;
        }

    } 
    } else { // Q20/MZT format

        if(qd_motor) {

            switch(qd_stage) {

                case 0:   // Preample
                    qd_data=qd_header[qd_count];

                    qd_count++;

                    if(qd_count==2) {
                        qd_data=qd_numblocks;

                    }
                    if(qd_count>=5) {
                        qd_stage=1;
                        qd_blockcount=0;
                    }

                    return qd_data;

                case 1: // 

                    if(qd_blockcount>=qd_numblocks) {
                        return 0x16;
                    }

                    qd_stage=2;
                    qd_count=0;

                    return 0xa5;

                case 2:  // Header

                    qd_count++;

                    if(qd_count==1) {
                        return 0;
                    }
                    if(qd_count==2) {
                        return 0x40;
                    }
                    if(qd_count==3) {
                        return 0;
                    }

                    if(qd_count==0x16) {
                        return 0;
                    }
                    if(qd_count==0x17) {
                        return 0;
                    }


                    lfs_file_read(&lfs,&qd_drive,&qd_data,1);
                    qd_ptr++;

                    if(qd_count==0x18) {
                        qd_blocksize=qd_data;
                    }
                    if(qd_count==0x19) {                    
                        qd_blocksize+=qd_data*256;
                    }

                    if(qd_count>=67) {
                        qd_stage=3;
                        qd_ptr+=66;
                        lfs_file_seek(&lfs,&qd_drive,qd_ptr,LFS_SEEK_SET);
                    }

                    return qd_data;

                case 3:  // Checksum
                    qd_stage=4;
                    return 0xff;

                case 4:  // Checksum
                    qd_stage=5;
                    return 0xff;

                case 5:  // Sync
                    qd_stage=6;
                    return 0x16;

                case 6:   // Preample
                    qd_stage=7;
                    qd_count=0;
                    return 0xa5;

                case 7:
                    qd_count++;
                    if(qd_count==1) return 5;
                    if(qd_count==2) return qd_blocksize&0xff;
                    if(qd_count==3) {
                        qd_stage=8;
                        qd_count=0;
                        return (qd_blocksize&0xff00)>>8;
                    }

                case 8:  // Data
                    lfs_file_read(&lfs,&qd_drive,&qd_data,1);
                    qd_ptr++;
                    qd_count++;
                    if(qd_count>=qd_blocksize) {
                        qd_stage=9;
                    }

                    return qd_data;

                case 9:  // Checksum
                    qd_stage=10;
                    return 0xff;

                case 10:  // Checksum
                    qd_stage=11;
                    return 0xff;

                case 11:  // Sync
                    qd_stage=1;
                    qd_blockcount+=2;
                    return 0x16;
            }

        }

    }

    return 0;

}

//// File select

int32_t qd_count_files() {

    lfs_dir_t lfs_dirs;
    struct lfs_info lfs_dir_info;
    uint32_t num_entry=0;

    int err= lfs_dir_open(&lfs,&lfs_dirs,"/");

    if(err) return -1;

    while(1) {

        int res= lfs_dir_read(&lfs,&lfs_dirs,&lfs_dir_info);
        if(res<=0) {
            break;
        }

        switch(lfs_dir_info.type) {

            case LFS_TYPE_DIR:
                break;
            
            case LFS_TYPE_REG:
                num_entry++;

                break;

            default:
                break; 

        }

    }

    lfs_dir_close(&lfs,&lfs_dirs);

    return num_entry;

}

int32_t qd_select_image(int32_t qd_image_num) {

    lfs_dir_t lfs_dirs;
    struct lfs_info lfs_dir_info;
    uint32_t num_entry=0;

    int err= lfs_dir_open(&lfs,&lfs_dirs,"/");

    if(err) return -1;

    while(1) {

        int res= lfs_dir_read(&lfs,&lfs_dirs,&lfs_dir_info);
        if(res<=0) {
            break;
        }

        switch(lfs_dir_info.type) {

            case LFS_TYPE_DIR:
                break;
            
            case LFS_TYPE_REG:

                if(num_entry==qd_image_num) {
                    strncpy(qd_filename,lfs_dir_info.name,64);
                }

                num_entry++;

                break;

            default:
                break; 

        }

    }

    lfs_dir_close(&lfs,&lfs_dirs);

    return num_entry;    


}

void draw_filename(){

    memset(status,0,80);

    for(int i=0;i<38;i++) {

        if(qd_filename[i]) {
            status[i]=asciitomz[qd_filename[i]];
        }

    }

    return;

}

//// Z80 Bus interface

static inline void io_write(uint16_t address, uint8_t data)
{

    uint8_t b;

    switch(address&0xff) {

        // MZ-1R23 & 1R24

        case 0xb8:  // Kanji Control
            kanjictl=data;
            return;

        case 0xb9:  // Kanji PTR
            dictptr=data+(address&0xff00);
            kanjiptr=dictptr<<5;

            return;

        // MZ-1R18

        case 0xea:  // RAMFILE WRITE 
            mz1r18[mz1r18ptr&0xffff]=data;
            mz1r18ptr++;
            return;

        case 0xeb:  // RAMFILE PTR
            mz1r18ptr=data+(address&0xff00);
            return;
        
        // MZ-1R12

        case 0xf8:  // MZ-1R12 ptr high 
            mz1r12ptr=(data<<8)+(mz1r12ptr&0xff);
            mz1r12ptr&=0x7fff;
            return;

        case 0xf9:  // MZ-1R12 ptr low
            mz1r12ptr=data+(mz1r12ptr&0xff00);
            return;

        case 0xfa:
//            mz1r12[mz1r12ptr&0xffff]=data;
            mz1r12[mz1r12ptr&0x7fff]=data;
            mz1r12ptr++;
            if(mz1r12ptr>0x7fff) { 
                mz1r12ptr=0;
            }
            return;

        //  MZ-1R12 control 

        case 0xba:
            rompage=data&0x3f;
            flash_command=0x10000000+(data&0x3f);
            return;

        case 0xbb:
            rompage=data&0x3f;
            flash_command=0x20000000+(data&0x3f);
            return;

        case 0xbc:
            emmpage=data&0xf;
            flash_command=0x30000000+(data&0xf);        
            return;

        case 0xbd:
            emmpage=data&0xf;
            flash_command=0x40000000+(data&0xf);        
            return;

        // EMM
        case 0:
            emmptr=(emmptr&0x7ff00)+(data);
            return;

        case 1:
            emmptr=(emmptr&0x700ff)+(data<<8);
            return;           

        case 2:
//            emmptr=(emmptr&0xffff)+(data<<16);
            emmptr=(emmptr&0xffff)+((data&7)<<16);
            return; 

        case 3:
            if(emmptr>0x4ffff) {
                emmptr-=0x50000;
            }
            emm[emmptr++]=data;
            return;

        // BANK control

        case 0xe1:  // BANK TO DRAM
            vram_enabled=0;
            return;

        case 0xe3:  // BANK TO VRAM
            vram_enabled=1;
            return;

        case 0xe4:
            vram_enabled=1;
            pcg_enabled=0xff;
            return;

        case 0xe5:  // BANK TO PCG
            pcg_enabled=data&3;
            return;

        case 0xe6:  // BANK FROM PCG
            pcg_enabled=0xff;
            return;

        // VGA Out

        case 0xf0:
            pcg_control=data;
            return;

        case 0xf1:
            pallet[(data&0x70)>>4]=data&7;
            return;

        // 

    
        case 0xf2: // PSG1
//            flash_command=0x50000000+data;     
    psg_write(0,data);
            return;

        case 0xf3: // PSG2
//            flash_command=0x60000000+data; 
    psg_write(1,data);
            return;


        case 0xe9: // PSG1+2
//            flash_command=0x70000000+data; 
        psg_write(0,data);
        psg_write(1,data);
            return;

        case 0xf6:

            if(sioa_access==0) {
                sioa[0]=data;
//                if(data&0x7)
                sioa_access=1;
            } else {
                if((sioa[0]&7)==2) {
//                printf("[sioa iv:%d]",data);
                }

                sioa[siob[0]&7]=data;
                sioa_access=0;
            }

            return;

        case 0xf7:

//       printf("[siob:%d,%x]",siob_access,siob[0]);


            if(siob_access==0) {
                siob[0]=data;
//            if(data&0x7)
                 siob_access=1;
            } else {

                if((siob[0]&7)==5) {
                    if(((siob[5]&0x80)==0)&&(data&0x80)) { // Motor ON
                        qd_motor=1;
                        qd_ptr=0;
                        qd_stage=0;
                        qd_count=0;
                        if(qd_status) {
                           flash_command=0x80000000; // start qd read
//                        lfs_file_seek(&lfs,&qd_drive,0,LFS_SEEK_SET);
                        }
                        qd_sync=1;
                    }
                }

                if((siob[0]&7)==2) {
//                printf("[siob iv:%x]",data);
                }


                siob[siob[0]&7]=data;
                siob_access=0;
            }

            return;

        case 0xfc: // PIOA

            if(pioa_next_mask) {
                pioa[3]=data;
                pioa_next_mask=0;
                return;
            }
            if(pioa_next_iocontrol) {
                pioa_next_iocontrol=0;
                return;
            }

            switch(data&0xf) {

                case 0:
                case 2:
                case 4:
                case 8:
                case 0xa:
                case 0xc:
                case 0xe:

                    pioa[0]=data;
                    return;
                
                case 3: // Intrrupt disable

                    if(data&0x80) {
                        pioa[2]|=0x80;
                    } else {
                        pioa[2]&=0x7f;
                    }

                    return;

                case 7: // Interrupt control

                    pioa[2]=data;
                    if(data&0x10) {
                        pioa_next_mask=1;
                    }
                    return;

                case 0xf: // Mode control

                    if((data&0xc0)==0xc0) { // Mode 3
                        pioa_next_iocontrol=1;
                    }

                default:                

                    return;
            }

        case 0xfd: // PIOB

            if(piob_next_mask) {
                piob[3]=data;
                piob_next_mask=0;
                return;
            }
            if(piob_next_iocontrol) {
                piob_next_iocontrol=0;
                return;
            }


            switch(data&0xf) {

                case 0:
                case 2:
                case 4:
                case 8:
                case 0xa:
                case 0xc:
                case 0xe:

                    piob[0]=data;
                    return;
                
                case 3: // Intrrupt disable

                    if(data&0x80) {
                        piob[2]|=0x80;
                    } else {
                        piob[2]&=0x7f;
                    }

                    return;

                case 7: // Interrupt control

                    piob[2]=data;
                    if(data&0x10) {
                        piob_next_mask=1;
                    }
                    return;

                case 0xf: // Mode control

                    if((data&0xc0)==0xc0) { // Mode 3
                        piob_next_iocontrol=1;
                    }

                default:                

                    return;
                }

                break;

        case 0xfe: // PIOA data

            pioa_data=data;
            pioa_data&=0xcf;

            break;

    }
 
    return;

}


static inline void memory_write(uint16_t address, uint8_t data)
{

    int b;
    
    if(address<0xd000) { // Main RAM
        return;
    }

    if((pcg_enabled!=0xff)&&(address<0xf000)) {

        if((pcg_enabled&3)!=0) {
            // PCG BANK
            pcg[(address-0xd000)+((pcg_enabled&3)-1)*0x2000]=data;

        }

    } else if((vram_enabled)&&(address<0xe000)) {

            vram[address&0xfff]=data;

    } else {

        if(vram_enabled) {

        switch(address) {



            // i8255 emulation (ONLY PORT C)
            case 0xe002:

                if(data&1) {
                    beep_mute=0;
                } else {
                    beep_mute=1;
                }

                if(data&4) {
                    i8253_enable_irq=1;
                } else {
                    i8253_enable_irq=0;
                }

                memioport[2]=data;

                break;

            case 0xe003:

                if((data&0x80)==0) { // Bit operation

                    b=(data&0x0e)>>1;


                    if(data&1) {
                        memioport[2]|= 1<<b;
                    } else {
                        memioport[2]&= ~(1<<b);
                    }
                }
                break;


            // i8253 emulation

            case 0xe004:
            case 0xe005:
            case 0xe006:

// TOO LONG ?

                int i8253_channel=address-0xe004;

                if((i8253[i8253_channel]&0x30)==0x30) {

                    if(i8253_access[i8253_channel]) {

                        i8253_preload[i8253_channel]&=0xff;
                        i8253_preload[i8253_channel]|=data<<8;
                        i8253_access[i8253_channel]=0;
                        i8253_counter[i8253_channel]=i8253_preload[i8253_channel];

                    } else {

                                i8253_preload[i8253_channel]&=0xff00;
                                i8253_preload[i8253_channel]|=data;
                                i8253_access[i8253_channel]=2;
                    }
                }

                if((i8253[i8253_channel]&0x30)==0x20) {
                    i8253_preload[i8253_channel]&=0xff;
                    i8253_preload[i8253_channel]|=data<<8;
                    i8253_counter[i8253_channel]=i8253_preload[i8253_channel];
                }

                if((i8253[i8253_channel]&0x30)==0x10) {
                    i8253_preload[i8253_channel]&=0xff00;
                    i8253_preload[i8253_channel]|=data;
                    i8253_counter[i8253_channel]=i8253_preload[i8253_channel];

                }

                i8253_pending[i8253_channel]=1;

//                        i8253_counter[addr-4]=i8253_preload[addr-4];

                break;

            case 0xe007:

                b=(data&0xc0)>>6;

                if(b!=3) {
                    if((data&0x30)==0) {
                        i8253_latch[b]=i8253_counter[b];
                        i8253_latch_flag=1;
                    }
                    i8253[b]=data;
                }

                break;


            case 0xe008:

                if(b&1) { // Beep ON
                    beep_flag=1;
                } else {  // Beep Off
                    beep_flag=0;
                }

                memioport[8]=data;

                break;

            // PCG 700

            case 0xe010:
                pcg700_data=data;
                return;

            case 0xe011:
                pcg700_ptr&=0x700;
                pcg700_ptr|=data;
                return;

            case 0xe012:
                if(pcg700_control&0x10) { // WE: 1 -> 0
                    if((data&0x10)==0) {
                        // write operaton

                        if(pcg700_control&0x20) {
                            // COPY CGROM
                            if(pcg700_ptr<0x400) {
                                pcg700[pcg700_ptr]=cgram[pcg700_ptr+0x400];
                            } else {
                                pcg700[pcg700_ptr]=cgram[pcg700_ptr+0x800];                                
                            }
                        } else {
                            pcg700[pcg700_ptr]=pcg700_data;
                        }
                    }
                } else {    // WE: 0 -> 1
                    if(data&0x10) {
                        // set high address
                        pcg700_ptr&=0xff;
                        pcg700_ptr|=(data&7)<<8;
                    }
                }
                pcg700_control=data;
                return;
        }


        }

    }

    return;

    }



void init_emulator(void) {

    // Erase Gamepad info

    kanjiptr=0;
    mz1r12ptr=0;
    mz1r18ptr=0;
    emmptr=0;
    pcg700_ptr=0;
    pcg700_control=0xff;
    pcg_control=0;

    vram_enabled=1;
    pcg_enabled=0xff;

    for(int i=0;i<8;i++) {
        pallet[i]=i;
    }

    memcpy(cgram,fontrom,0x1000);
    memcpy(extram,extrom,0x1800);

    // Load DUMMY data to MZ-1R12

//    memcpy(mz1r12,romslots,0x8000);
    memset(mz1r12,0xaa,0x8000);

    // Load EMM data to EMM

    memcpy(emm,emmslots,0x50000);

    psg_reset(0);
    psg_reset(1);

}

// Main thread (Core1)

void __not_in_flash_func(main_core1)(void) {

    volatile uint32_t bus;

    uint32_t control,address,data,response;
    uint32_t needwait=0;
    uint64_t videosync;

//    multicore_lockout_victim_init();

    gpio_init_mask(0xffffffff);
    gpio_set_dir_all_bits(0x00000000);  // All pins are INPUT
//  EXINT
    gpio_set_dir(24,true);
    gpio_put(24,true);
//  EXWAIT    
    gpio_init(32);
    gpio_set_dir(32,false);

    //    gpio_set_dir(32,true);

    while(1) {

        bus=gpio_get_all();

        control=bus&0xf4000000;

        // Check IO Read

        if(control==0x54000000) {

            address=(bus&0xffff00)>>8;

            switch(address&0xff) {

                // MZ-1R23 & 1R24

                case 0xb9:
                    // ENABLE EXWAIT
                    gpio_set_dir(32,true);
                    gpio_put(32,false);
                    needwait=1;

                    if(kanjictl&0x80) {
                        // KANJI
                        data=kanjirom[kanjiptr++];
                        if(kanjictl&0x40) { // Bit reverse
                            data=bitreverse[data];           
                        }
                        response=1;
                        break;

                    } else {
                        data=jishorom[((kanjictl&3)<<16)+(dictptr++)];
                        if(kanjictl&0x40) { // Bit reverse
                            data=bitreverse[data];           
                        }
                        response=1;
                        break;
                    }

                // MZ-1R18

                case 0xea:
                    data=mz1r18[mz1r18ptr&0xffff];
                    mz1r18ptr++;
                    response=1;
                    break;

// DEBUG

                case 0xeb:
                    data=mz1r18ptr&0xff;
                    response=1;
                    break;


                // MZ-1R12

                case 0xf8:
                    mz1r12ptr=0;
                    data=0;
                    response=1;
                    break;

                case 0xf9:
//                    data=mz1r12[mz1r12ptr&0xffff];
                    data=mz1r12[mz1r12ptr&0x7fff];
                    mz1r12ptr++;
                    if(mz1r12ptr>0x7fff) {
                        mz1r12ptr=0;
                    }
                    response=1;
                    break;

                // EMM
                case 0x3:
                    if(emmptr>0x4ffff) {
                        emmptr-=0x50000;
                    }
                    data=emm[emmptr++];
                    response=1;
                    break;

                case 0xf4: // SIOA DATA

//                    qd_data=qd_read();

 //           printf("[%02x/%05x/%05x]",qd_data,qd_ptr,qd_blocksize);
 //           printf("[%02x]",qd_data);

                    data=qd_data;
                    qd_data_ready=0;
                    qd_data_request=1;

                    response=1;
                    break;

                case 0xf6: // SIOA

                    response=1;
                    if(sioa_access) {

                        sioa_access=0;

                        switch(sioa[0]&7) {

                            case 0:
                            // CTS : Write Protect
                            // DCD : Media present

                            if(qd_status==1) {
                                if(qd_data_ready) {
                                    data=0x2d;
                                } else {
                                    data=0x2c;
                                }
                            } else {
                                data=0x24;
                            }

                            break;

                            case 1:
                                data=1;
                                break;
                            case 2:
                                data=sioa[2];
                                break;
                            default:
                                data=0xff;
                            }

                    } else {

                        if(qd_status==1) {
                            if(qd_data_ready) {
                                data=0x2d;
                            } else {
                                data=0x2c;
                            }
                        } else {
                            data=0x24;
                        }

                    }

                    break;

                case 0xf7: // SIOB

                    response=1;
                    if(siob_access) {

                        siob_access=0;

                        switch(siob[0]&7) {

                        case 0:

                        // DCD : Home position

                            if(qd_status==1) {
                                data=0xd;
                            } else {
                                data=0x4;
                            }
                            break;
                        case 1:
                            data=1;
                            break;
                        case 2:
                            data=siob[2];
                            break;
                        default:
                            data=0xff;
                        }

                    } else {
                        data=0xff;
                    }

                    break;

                case 0xfe: // PIO A data

                    data=pioa_data;
                    response=2;
                    break;

                default:
                    response=0;

            }

            if(response) {

                if(needwait) {
                    gpio_put(32,true);
                    gpio_set_dir(32,false);
                    needwait=0;
                }


                if(response==2) {
                // Set GP4-5 to OUTPUT

                    gpio_set_dir_masked(0x30,0x30);                     

                } else {

                    // Set GP0-7 to OUTPUT

                    gpio_set_dir_masked(0xff,0xff);

                }


                gpio_put_masked(0xff,data);

                // Wait while RD# is low

                control=0;

                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

                // Set GP0-7 to INPUT

                gpio_set_dir_masked(0xff,0x00);

            } else {

                // Wait while RD# is low
                control=0;

                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

            }

            continue;
        }

        // Check IO Write

        else if(control==0x94000000) {

            address=(bus&0xffff00)>>8;
            data=bus&0xff;
#ifdef PSGWAIT
            needwait=0;
            switch (address&0xff)
            {
                case 0xe9:
                case 0xf2:
                case 0xf3:
                    gpio_set_dir(32,true);
                    gpio_put(32,false);
                    needwait=1;
                    break;
                default:
                    needwait=0;
            }
#endif

            io_write(address,data);

#ifdef PSGWAIT
            if(needwait) {
                gpio_put(32,true);
                gpio_set_dir(32,false);   
                needwait=0; 
            }
#endif

            control=0;

            // Wait while WR# is low
            while(control==0) {
                bus=gpio_get_all();
                control=bus&0x40000000;
            }

            continue;
        }
        
        // check Memory write

        else if(control==0xa4000000) {

            address=(bus&0xffff00)>>8;
            data=bus&0xff;

#ifdef PCGWAIT
            needwait=0;
            if((pcg_enabled!=0xff)&&(address<0xf000)&&(address>=0xd000)) {
                if((pcg_enabled&3)!=0) {
                    gpio_set_dir(32,true);
                    gpio_put(32,false);
                    needwait=1;
                }
            }
#endif


            memory_write(address,data);

#ifdef PCGWAIT
            if(needwait) {
                // videosync=gpio_get_all64();
                // while((videosync&0x600000000)==0x600000000) {
                //     videosync=gpio_get_all64();
                // }
  
//                if(gpio_get(34)!=0) {
//                if((gpio_get_all64()&0x600000000u)==0x600000000) {
                    busy_wait_at_least_cycles(600); // 3.3 * clocks
//                }

                gpio_put(32,true);
                gpio_set_dir(32,false);   
                needwait=0; 
            }
#endif

            control=0;

            // Wait while WR# is low
            while(control==0) {
                bus=gpio_get_all();
                control=bus&0x40000000;
            }


        } 

        // check Memory read

        else if((control==0x64000000)||(control==0x60000000)) {

            address=(bus&0xffff00)>>8;
            response=0;

            //
            if(address>=0xd000) { // 

                if((pcg_enabled!=0xff)&&(address<0xf000)) {

                    if((pcg_enabled&3)!=0) {
                // PCG BANK
                        data=pcg[(address-0xd000)+((pcg_enabled&3)-1)*0x2000];
                    } else {
                        data=cgram[(address-0xd000)&0xfff];
                    }

                    response=1;

                } else if((vram_enabled)&&(address>=0xe800)) {

                    // if((control&0x400000)==0) {
                    //     // ENABLE EXWAIT
                    //     gpio_set_dir(32,true);
                    //     gpio_put(32,false);
                    //     needwait=1;
                    // }

                    data=extram[address-0xe800];
                    response=1;

                }

            }

            if(response) {

                // Set GP0-7 to OUTPUT

                gpio_set_dir_masked(0xff,0xff);

                gpio_put_masked(0xff,data);

                // if(needwait) {
                //     gpio_put(32,true);
                //     gpio_set_dir(32,false);
                //     needwait=0;
                // }

                // Wait while RD# is low

                control=0;

                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

                // Set GP0-7 to INPUT

                gpio_set_dir_masked(0xff,0x00);

            } else {

                control=0;

                // Wait while RD# is low
                while(control==0) {
                    bus=gpio_get_all();
                    control=bus&0x80000000;
                }

            }

        } 

        // Interrupt vector

        else if(control==0xd0000000) {

            control=0;

            gpio_set_dir_masked(0xff,0xff);

            if(pioa_enable_irq) {
                gpio_put_masked(0xff,pioa[0]);                
                pioa_enable_irq=0;
            }

            gpio_put(24,true); // Release EXINT

            // Wait while M1 is low
            while(control==0) {
                bus=gpio_get_all();
                control=bus&0x04000000;
            }

            gpio_set_dir_masked(0xff,0x00);

        }

    }
        
}

int main() {

    uint32_t menuprint=0;
    uint32_t filelist=0;
    uint32_t subcpu_wait;
    uint32_t rampacno;
    uint32_t pacpage;

    static uint32_t hsync_wait,vsync_wait;

    vreg_set_voltage(VREG_VOLTAGE_1_20);  // for overclock to 300MHz
    set_sys_clock_khz(300000 ,true);

    initVGA();

    multicore_launch_core1(main_core1);

    init_emulator();

    irq_set_exclusive_handler (PIO0_IRQ_0, hsync_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled (pio0, pis_interrupt0 , true);

    // Set RESET# interrupt

//    gpio_set_irq_enabled_with_callback(RESET_PIN,GPIO_IRQ_EDGE_FALL,true,z80reset);
    gpio_set_irq_enabled_with_callback(RESET_PIN,GPIO_IRQ_EDGE_RISE,true,z80reset);


    // Mount LittleFS (8MiB)

    // mount littlefs
    if(lfs_mount(&lfs,&PICO_FLASH_CFG)!=0) {
       // format
       lfs_format(&lfs,&PICO_FLASH_CFG);
       lfs_mount(&lfs,&PICO_FLASH_CFG);
   }

   // Initialize buttons

   gpio_init(45);
   gpio_init(46);
   gpio_set_dir(45,false);
   gpio_set_dir(46,false);
   gpio_set_pulls(45,true,false);
   gpio_set_pulls(46,true,false);

   // BEEP

    gpio_set_function(47,GPIO_FUNC_PWM);
    pwm_slice_num = pwm_gpio_to_slice_num(47);

    pwm_set_wrap(pwm_slice_num, 256);
    pwm_set_chan_level(pwm_slice_num, PWM_CHAN_B, 0);
    pwm_set_enabled(pwm_slice_num, true);

    qdimage_numfiles=qd_count_files();
    qdimage_selected=0;

    while(1) {

        if(flash_command!=0) {

            switch(flash_command&0xf0000000) {

                case 0x10000000:  // SRAM load
                    memcpy(mz1r12,romslots+((flash_command&0x3f))*0x8000u,0x8000);
                    break;

                case 0x20000000:  // SRAM Save

                    for(uint32_t i=0;i<0x8000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_erase(i+0x80000u+((flash_command&0x3f))*0x8000u, 4096);  
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    for(uint32_t i=0;i<0x8000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_program(i+0x80000u+((flash_command&0x3f))*0x8000u, (const uint8_t *)(mz1r12+i), 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    break;

                case 0x30000000:  // EMM load
                    memcpy(emm,emmslots+(flash_command&0xf)*0x50000u,0x50000);
                    break;                    

                case 0x40000000:  // EMM Save

                    for(uint32_t i=0;i<0x50000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_erase(i+0x200000u+(flash_command&0xf)*0x50000u, 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                    for(uint32_t i=0;i<0x50000;i+=4096) {
                    uint32_t ints = save_and_disable_interrupts();
//                    multicore_lockout_start_blocking();     // pause another core
                    flash_range_program(i+0x200000u+(flash_command&0xf)*0x50000u, (const uint8_t *)(emm+i), 4096);
//                    multicore_lockout_end_blocking();
                    restore_interrupts(ints);
                    }

                case 0x50000000:  // PSG1
                    psg_write(0,flash_command&0xff);
                    break;

                case 0x60000000:  // PSG2
                    psg_write(1,flash_command&0xff);
                    break;

                case 0x70000000:  // PSG1+2
                    psg_write(0,flash_command&0xff);
                    psg_write(1,flash_command&0xff);
                    break;

                case 0x80000000:  // QD ON
                    if(qd_status) {
                        lfs_file_seek(&lfs,&qd_drive,0,LFS_SEEK_SET);
                        qd_data_request=1;

                    }


                    break;

                case 0x90000000:  // QD Read
                    break;

                case 0xa0000000:  // QD Write
                    break;                    


                    break;  

            }
            flash_command=0;

        }

        // EXINT

        if(pioa_enable_irq) {
            gpio_put(24,false);
        }

        // if(qd_motor) {
        //     status[39]=0x6b;
        // } else {
        //     status[39]=0;
        // }

        // QD read

        if((qd_motor)&&(qd_data_request)) {
            qd_data=qd_read();
            qd_data_request=0;
            qd_data_ready=1;
        }

        // UI 

        if(button1_pressed) {

            if(qd_status) {
                lfs_file_close(&lfs,&qd_drive);
                qdimage_selected++;
                if(qdimage_selected>=qdimage_numfiles) {
                    qdimage_selected=0;
                }                
            }

            qd_select_image(qdimage_selected);
            draw_filename();

            lfs_file_open(&lfs,&qd_drive,qd_filename,LFS_O_RDONLY);

            qd_check();
            qd_status=1;
            button1_pressed=0;
            qdimage_display_count=QDIMAGE_TIME;

        }

        if(button2_pressed) {

            if(qd_status) {
                lfs_file_close(&lfs,&qd_drive);
                if(qdimage_selected==0) {
                    qdimage_selected=qdimage_numfiles;
                }
                qdimage_selected--;                                  
            }

            qd_select_image(qdimage_selected);
            draw_filename();

            lfs_file_open(&lfs,&qd_drive,qd_filename,LFS_O_RDONLY);

            qd_check();
            qd_status=1;
            button2_pressed=0;
            qdimage_display_count=QDIMAGE_TIME;

        }

        if(button1_long) {
            if(qd_status) {
                lfs_file_close(&lfs,&qd_drive);
                qd_status=0;
                button1_long=0;
            }
            memset(status,0,80);
        }

    }
}

