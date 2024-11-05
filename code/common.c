#include <stdio.h>
#include <stdlib.h>
#include"common.h"

/*
The verbose variable is defined and initialized with the value ALL, indicating that
all log levels are enabled by default. This variable can be modified to control
the level of detail in log messages.
*/
int verbose = ALL;


/*
error function - wrapper for perror

Definition of error function. It takes a string msg as an argument, which represents
an error message. 
*/
void error(char *msg) {
    //print the error message along with the error description corresponding
    //to the current value of errno
    perror(msg);
    
    //exits with a status code of 1 to indicate an error.
    exit(1);
}




