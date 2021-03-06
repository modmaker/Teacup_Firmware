
#include <stdio.h>
#include <stdlib.h>

#define __AVR_ATmega1280__	1
#define	SKIP_ARDUINO_INC	1

// Prevent inclusion of "arduino.h" contents in config.h
#define _ARDUINO_H

#include "config.h"
#include "dda.h"
#include "home.h"


#define PRINT_INT_XYZ( macro, units) printf( "   %-25s  %10d     %10d     %10d            n/a          %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, units);
#define PRINT_INT_XYZE( macro, units) printf( "   %-25s  %10d     %10d     %10d     %10d          %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, macro ## E, units);
#define PRINT_LONG_XYZ( macro, units) printf( "   %-25s  %10ld     %10ld     %10ld            n/a          %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, units);
#define PRINT_LONG_XYZE( macro, units) printf( "   %-25s  %10ld     %10ld     %10ld     %10ld          %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, macro ## E, units);
#define PRINT_FLOAT_XYZE( macro, units) printf( "   %-25s      %10.3f     %10.3f     %10.3f     %10.3f      %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, macro ## E, units);
#define PRINT_FLOAT_XYZ( macro, units) printf( "   %-25s      %10.3f     %10.3f     %10.3f        n/a          %s\n", \
		# macro, macro ## X, macro ## Y, macro ## Z, units);

void main( void)
{
	printf( "\nThese are the actual values as defined in config.h and other header files:\n\n");
	printf( "                                   X-axis         Y-axis         Z-axis         E-axis           units\n");
	printf( "mechanical:\n");
	PRINT_FLOAT_XYZ( AXIS_TRAVEL_, "[mm]");
	PRINT_INT_XYZE( MICROSTEPPING_, "[-]");
	
	printf( "config.h:\n");
	PRINT_INT_XYZE( STEPS_PER_REV_, "[steps/rev]");
	PRINT_FLOAT_XYZE( FEED_PER_REV_, "[mm/rev]");
	PRINT_FLOAT_XYZE( STEPS_PER_MM_, "[steps/mm]");
	PRINT_FLOAT_XYZE( MAX_REV_SPEED_, "[rev/sec]");
	PRINT_INT_XYZE( MAX_STEP_FREQ_, "[steps/sec]");
	PRINT_INT_XYZE( MAXIMUM_FEEDRATE_, "[mm/min]");
	PRINT_INT_XYZ( SEARCH_FEEDRATE_, "[mm/min]");
	PRINT_INT_XYZE( MIN_CLOCKS_PER_STEP_, "[clocks/step]");

	printf( "dda.h:\n");
	PRINT_LONG_XYZE( UM_PER_STEP_, "[um/step]");

	printf( "homing speeds:\n");
	PRINT_INT_XYZ( HOME_FEED_FAST_, "[mm/min]");
	PRINT_INT_XYZ( HOME_FEED_SLOW_, "[mm/min]");
	PRINT_INT_XYZ( HOME_FAST_STEP_PERIOD_, "[us]");
	PRINT_INT_XYZ( HOME_SLOW_STEP_PERIOD_, "[us]");

	printf( "\n");
	exit( 0);
}


