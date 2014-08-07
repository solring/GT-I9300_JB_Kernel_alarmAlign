Enable aligning RTC alarms through /proc entry

Kernel: GT-I9300_JB_Opensource_Update9

---------------------------------------------------

################################################################################


How to Build

	- get Toolchain
		From android git server , codesourcery and etc ..
		 - arm-eabi-4.4.3

	- edit Makefile

		edit "CROSS_COMPILE" to right toolchain path(You downloaded).
		EX)  CROSS_COMPILE= $(android platform directory you download)/android/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3/bin/arm-eabi-
          	Ex)  CROSS_COMPILE=/usr/local/toolchain/arm-eabi-4.4.3/bin/arm-eabi-          // check the location of toolchain

	$ make arch=arm m0_00_defconfig
	$ make



Output files

	- Kernel : arch/arm/boot/zImage
	- module : drivers/*/*.ko



How to Clean	

	$ make clean

################################################################################

----------------------------------------------------------------------------

Usage: echo [align period in second] > /proc/aligned-alarm


