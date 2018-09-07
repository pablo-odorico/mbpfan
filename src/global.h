#ifndef _GLOBAL_H_
#define _GLOBAL_H_

extern int daemonize;
extern int verbose;

extern const char* PROGRAM_NAME;
extern const char* PROGRAM_PID;

struct s_sensors {
    FILE* file;
    char* path;
    unsigned int temperature;
    struct s_sensors *next;
};

struct s_fans {
    char* name;
    FILE* file;
    char* path;  // TODO: unused
    char* fan_output_path;
    char* fan_manual_path;
    int old_speed;
    float speed_ratio;
    int max_speed;
    int min_speed;
    int fan_id; // applesmc.768/fan#_*
    struct s_fans *next;
};

typedef struct s_sensors t_sensors;
typedef struct s_fans t_fans;

extern t_sensors* sensors;
extern t_fans* fans;

// Logging
#define LOG_TYPE(type, format, ...) \
    do { \
        if (daemonize) syslog(type, format "\n", ##__VA_ARGS__); \
        printf(format "\n", ##__VA_ARGS__); \
        if (type == LOG_CRIT) exit(1); \
    } while (false)

#define LOG(format, ...) LOG_TYPE(LOG_INFO, format, ##__VA_ARGS__)
#define FAIL(format, ...) LOG_TYPE(LOG_CRIT, format, ##__VA_ARGS__)

#endif