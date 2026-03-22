/*
 * collect.h — Platform-independent data collection interface
 *
 * Defines the data structures and functions that platform-specific
 * implementations (collect_esxi.c, collect_linux.c) must provide.
 * bmc.c calls these functions to populate BMC parameters.
 */

#ifndef COLLECT_H
#define COLLECT_H

#include <stdint.h>

#define CACHE_SIZE 256
#define MAX_DIMMS  16
#define MAX_DRIVES 8
#define MAX_NICS   4

/* ------------------------------------------------------------------ */
/*  Data structures                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char firmware[CACHE_SIZE];     /* Boot ROM / BIOS version */
    char hostname[CACHE_SIZE];     /* Short hostname */
    char fqdn[CACHE_SIZE];        /* Fully qualified domain name */
    char os_product[CACHE_SIZE];   /* "VMware ESXi" / "Proxmox VE" */
    char os_version[CACHE_SIZE];   /* "6.5.0" / "8.1" */
    char os_update[CACHE_SIZE];    /* "Update 3" / "" */
    char os_build[CACHE_SIZE];     /* "Build 20502893" / "" */
    char cpu_model[CACHE_SIZE];    /* "Intel(R) Xeon(R) CPU W5590" */
    int  cpu_packages;             /* Number of physical sockets */
    int  cpu_cores;                /* Total cores */
    int  cpu_speed_mhz;            /* CPU speed in MHz */
    char serial[CACHE_SIZE];       /* Machine serial number */
    char model[CACHE_SIZE];        /* Machine model ("Xserve3,1") */
    uint32_t total_ram_mb;         /* Total installed RAM in MB (rounded to GB) */
} system_info_t;

typedef struct {
    uint8_t  config_type;          /* 0x00 = populated */
    uint8_t  ecc_type;             /* 0x00 = no ECC display */
    uint32_t size_mb;
    char     slot_name[32];        /* "A1", "B3", "DIMM 1" */
    char     speed[16];            /* "1066 MHz" */
    char     type[16];             /* "DDR3", "DDR4" */
} dimm_info_t;

typedef struct {
    uint32_t capacity_mb;          /* SM reads as int32 via numberWithInt: */
    uint32_t reserved;             /* Must be 0 */
    char     model[32];
    char     kind[8];              /* "SSD" or "HDD" */
    char     iface[16];            /* "SATA", "SAS", "NVMe", "Fibre Channel" */
    char     vendor[32];           /* Manufacturer */
    char     location[16];         /* "Bay 1" */
    char     display[32];          /* Readable summary for logging */
} drive_info_t;

typedef struct {
    char name[16];                 /* "vmnic0" / "eth0" */
    char mac[24];                  /* "00:24:36:f3:31:ae" */
    char driver[32];               /* "e1000e" / "igb" */
} nic_static_t;

typedef struct {
    uint32_t packets_in;
    uint32_t packets_out;
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t reserved;
    char     ipv4[20];             /* "192.168.1.100" */
    char     netmask[20];          /* "255.255.255.0" */
    char     link[16];             /* "active" / "inactive" */
    char     mbps[16];             /* "1000" */
    char     duplex[8];            /* "Full" / "Half" */
} nic_dynamic_t;

/* ------------------------------------------------------------------ */
/*  Collection functions — implemented per platform                    */
/* ------------------------------------------------------------------ */

/*
 * Collect all system-level info (hostname, OS, CPU, serial, RAM total).
 * Called once at startup.
 */
int collect_system_info(system_info_t *info);

/*
 * Collect per-DIMM memory info from SMBIOS.
 * Returns number of populated DIMMs found.
 */
int collect_dimm_info(dimm_info_t *dimms, int max_dimms);

/*
 * Collect per-drive info.
 * Returns number of drives found.
 */
int collect_drive_info(drive_info_t *drives, int max_drives);

/*
 * Collect per-NIC static info (name, MAC, driver).
 * Returns number of NICs found.
 */
int collect_nic_static(nic_static_t *nics, int max_nics);

/*
 * Collect per-NIC dynamic info (IP, link, speed, duplex).
 * Uses NIC names from the static array to query the correct interfaces.
 */
int collect_nic_dynamic(nic_dynamic_t *dynamic, const nic_static_t *nics_static,
                        int nic_count);

/*
 * Check if drive configuration changed since last call.
 * Returns 1 if changed, 0 if same, -1 on error.
 */
int detect_drive_changes(void);

/*
 * Check if network configuration changed since last call.
 * Returns 1 if changed, 0 if same, -1 on error.
 */
int detect_network_changes(void);

#endif /* COLLECT_H */
