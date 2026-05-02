// Bottom Outer

difference() {

translate([-50,-30,-10]) {    
cube([100,60,10]);
}


translate([-48,-28,-8]) {  
cube([96,56,8]);
}

translate([-48,-30,-2]) {       
cube([96,56,2]);
    
}

// LINE OUT

translate([-1,25,-2]) {

cube([10,6,2]);

}


translate([-41.5,-23,-10]) {
    
cylinder(6,1.5,1.5);

}

translate([+41.5,-23,-10]) {
        
cylinder(6,1.5,1.5);

}

}


// Bottom Hole 1

translate([-41.5,-23,-8]) {

difference() {
    
cylinder(6,3.5,3.5);    
    
cylinder(6,1.5,1.5);

}
   
}

// Botom Hole 2

translate([+41.5,-23,-8]) {

difference() {
    
cylinder(6,3.5,3.5);    
    
cylinder(6,1.5,1.5);

}
   
}

// Support

translate([-30,14,-8]) {
    

cylinder(6,3,3);

}

translate([30,14,-8]) {
    

cylinder(6,3,3);

}