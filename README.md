
Kernel Source for Samsung Galaxy Ace 2 (GT-I8160)

=======================================================================================

 Compiling the source:

  1. Please download ARM toolchains first

  2. Go to /kernel and Set the propertes in build.sh and Makefile in order to match your kernel files' path

  3. Run the build script

     $ bash build.sh 


 The script will generate a zImage and will also create a odin flasheable package
 and it will put the files in a folder named 'out'.


 Then you can choose to 'dd' the zImage (named kernel.bin.md5) to mmcblk0p15
 or use odin and flash the tar file.


