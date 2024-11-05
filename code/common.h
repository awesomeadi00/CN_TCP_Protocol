/*
include guards ensuring that the contents of the header file are included only once
in each compilation unit.
*/
#ifndef COMMON_H_INCLUDED
#define COMMON_H_INCLUDED

//used to control the verbosity level of logging
extern int verbose;


/*
Constants related to logging levels. These constants use bitwise OR to combine
multiple log levels. 

NONE: No logging.
INFO: Informational messages.
WARNING: Warning messages.
DEBUG: Debugging messages.
ALL: All logging levels combined.
*/
#define NONE    0x0
#define INFO    0x1 
#define WARNING 0x10
#define INFO    0x1 
#define DEBUG   0x100
#define ALL     0x111

/*
The VLOG macro is defined to conditionally print log messages based on the specified
log level (level) and the verbosity level (verbose). It uses variadic macros (...)
to accept a variable number of arguments for the log message.
*/
#define VLOG(level, ... ) \
    if(level & verbose) { \
        fprintf(stderr, ##__VA_ARGS__ );\
        fprintf(stderr, "\n");\
    }\

//function prototype defined, used for reporting errors
void error(char *msg);


#endif

