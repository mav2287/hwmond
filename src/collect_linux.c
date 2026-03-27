/*
 * collect_linux.c — Linux-specific data collection for hwmond
 *
 * Collects system information from /proc, /sys, dmidecode, and
 * standard Linux tools. Implements the collect.h interface.
 * Works on any Linux distro (Proxmox, Debian, Ubuntu, RHEL, etc.)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "collect.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int read_file_trimmed(const char *path, char *buf, int bufsz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    buf[0] = '\0';
    if (fgets(buf, bufsz, fp)) {
        /* Trim trailing whitespace */
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'
                           || buf[len-1] == ' '))
            buf[--len] = '\0';
        /* Trim leading whitespace */
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (p != buf)
            memmove(buf, p, strlen(p) + 1);
    }
    fclose(fp);
    return strlen(buf);
}

static int popen_line(const char *cmd, char *buf, int bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    buf[0] = '\0';
    if (fgets(buf, bufsz, fp)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
    }
    pclose(fp);
    return strlen(buf);
}

/* ------------------------------------------------------------------ */
/*  System info                                                        */
/* ------------------------------------------------------------------ */

int collect_system_info(system_info_t *info)
{
    memset(info, 0, sizeof(*info));

    /* Firmware / BIOS version */
    read_file_trimmed("/sys/class/dmi/id/bios_version", info->firmware, CACHE_SIZE);

    /* Hostname */
    gethostname(info->hostname, CACHE_SIZE - 1);

    /* FQDN — try getaddrinfo, fall back to hostname */
    if (popen_line("hostname -f 2>/dev/null", info->fqdn, CACHE_SIZE) <= 0)
        strncpy(info->fqdn, info->hostname, CACHE_SIZE - 1);

    /* OS info — check for Proxmox first, then fall back to /etc/os-release */
    {
        int is_proxmox = 0;
        char pve_ver[64] = {0};

        /* Proxmox stores its version in the pve-manager package */
        if (popen_line("dpkg-query -W -f='${Version}' pve-manager 2>/dev/null",
                       pve_ver, sizeof(pve_ver)) > 0) {
            is_proxmox = 1;
            strncpy(info->os_product, "Proxmox VE", CACHE_SIZE - 1);
            /* pve_ver is like "8.1.4" — use major.minor as version */
            char *dot2 = strchr(pve_ver, '.');
            if (dot2) dot2 = strchr(dot2 + 1, '.');
            if (dot2) *dot2 = '\0'; /* truncate to major.minor */
            strncpy(info->os_version, pve_ver, CACHE_SIZE - 1);
        }

        FILE *fp = fopen("/etc/os-release", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (!is_proxmox && strncmp(line, "NAME=", 5) == 0) {
                    char *p = line + 5;
                    if (*p == '"') p++;
                    int l = strlen(p);
                    while (l > 0 && (p[l-1]=='\n'||p[l-1]=='"'||p[l-1]==' '))
                        p[--l] = '\0';
                    strncpy(info->os_product, p, CACHE_SIZE - 1);
                }
                else if (!is_proxmox && strncmp(line, "VERSION_ID=", 11) == 0) {
                    char *p = line + 11;
                    if (*p == '"') p++;
                    int l = strlen(p);
                    while (l > 0 && (p[l-1]=='\n'||p[l-1]=='"'||p[l-1]==' '))
                        p[--l] = '\0';
                    strncpy(info->os_version, p, CACHE_SIZE - 1);
                }
                else if (strncmp(line, "VERSION=", 8) == 0 && !info->os_build[0]) {
                    char *p = line + 8;
                    if (*p == '"') p++;
                    int l = strlen(p);
                    while (l > 0 && (p[l-1]=='\n'||p[l-1]=='"'||p[l-1]==' '))
                        p[--l] = '\0';
                    strncpy(info->os_build, p, CACHE_SIZE - 1);
                }
            }
            fclose(fp);
        }
    }

    /* CPU info from /proc/cpuinfo */
    {
        FILE *fp = fopen("/proc/cpuinfo", "r");
        if (fp) {
            char line[256];
            int max_pkg = -1;
            int cores_per_pkg = 0;

            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "model name", 10) == 0 && !info->cpu_model[0]) {
                    char *p = strchr(line, ':');
                    if (p) {
                        p++;
                        while (*p == ' ') p++;
                        int l = strlen(p);
                        while (l > 0 && (p[l-1]=='\n'||p[l-1]=='\r')) p[--l]='\0';
                        /* Collapse multiple spaces (Apple SMBIOS has them) */
                        char *src = p, *dst = p;
                        int prev_space = 0;
                        while (*src) {
                            if (*src == ' ') {
                                if (!prev_space) *dst++ = ' ';
                                prev_space = 1;
                            } else {
                                *dst++ = *src;
                                prev_space = 0;
                            }
                            src++;
                        }
                        *dst = '\0';
                        /* Strip "@ X.XX GHz" suffix — SM shows speed from binary */
                        char *at = strstr(p, " @");
                        if (at) *at = '\0';
                        l = strlen(p);
                        while (l > 0 && p[l-1] == ' ') p[--l] = '\0';
                        strncpy(info->cpu_model, p, CACHE_SIZE - 1);

                        /* Extract MAX speed from the "@ X.XX GHz" we just stripped */
                        if (at) {
                            char *ghz = at + 2;
                            while (*ghz == ' ') ghz++;
                            float fghz = atof(ghz);
                            if (fghz > 0)
                                info->cpu_speed_mhz = (int)((fghz * 1000 + 5) / 10) * 10;
                        }
                    }
                }
                else if (strncmp(line, "physical id", 11) == 0) {
                    char *p = strchr(line, ':');
                    if (p) {
                        int pkg = atoi(p + 1);
                        if (pkg > max_pkg) max_pkg = pkg;
                    }
                }
                else if (strncmp(line, "cpu cores", 9) == 0 && cores_per_pkg == 0) {
                    char *p = strchr(line, ':');
                    if (p) cores_per_pkg = atoi(p + 1);
                }
            }
            fclose(fp);

            info->cpu_packages = max_pkg + 1;
            if (info->cpu_packages <= 0) info->cpu_packages = 1;
            info->cpu_cores = cores_per_pkg * info->cpu_packages;
            if (info->cpu_cores <= 0) info->cpu_cores = 1;

            /* If speed wasn't found from model name, try lscpu */
            if (info->cpu_speed_mhz <= 0) {
                FILE *fp2 = popen("lscpu 2>/dev/null | grep 'CPU max MHz'", "r");
                if (fp2) {
                    char buf[128];
                    if (fgets(buf, sizeof(buf), fp2)) {
                        char *p = strchr(buf, ':');
                        if (p) info->cpu_speed_mhz = (int)(atof(p + 1) + 0.5);
                    }
                    pclose(fp2);
                }
            }
            /* Last resort: current frequency (may be power-saving reduced) */
            if (info->cpu_speed_mhz <= 0) {
                fp = fopen("/proc/cpuinfo", "r");
                if (fp) {
                    while (fgets(line, sizeof(line), fp)) {
                        if (strncmp(line, "cpu MHz", 7) == 0) {
                            char *p = strchr(line, ':');
                            if (p) {
                                info->cpu_speed_mhz = (int)(atof(p + 1) + 0.5);
                                break;
                            }
                        }
                    }
                    fclose(fp);
                }
            }
        }
    }

    /* Serial / Model from /sys/class/dmi/id/ */
    read_file_trimmed("/sys/class/dmi/id/product_serial", info->serial, CACHE_SIZE);
    read_file_trimmed("/sys/class/dmi/id/product_name", info->model, CACHE_SIZE);
    /* Fallback to dmidecode if /sys files are empty */
    if (!info->serial[0])
        popen_line("dmidecode -s system-serial-number 2>/dev/null", info->serial, CACHE_SIZE);
    if (!info->model[0])
        popen_line("dmidecode -s system-product-name 2>/dev/null", info->model, CACHE_SIZE);

    /* Total RAM from /proc/meminfo */
    /* Total RAM from SMBIOS (physical installed), not /proc/meminfo
     * (which reports kernel-available, ~3 GB less on a 192 GB machine).
     * dmidecode -t 17 gives per-DIMM sizes; sum them for true total.
     * Falls back to /proc/meminfo if dmidecode fails. */
    {
        FILE *fp = popen("dmidecode -t 17 2>/dev/null | awk '/^\\tSize:.*[0-9]/ && !/No Module/{v=$2; u=$3; if(u==\"GB\")v*=1024; s+=v} END{print s}'", "r");
        if (fp) {
            char buf[32] = {0};
            if (fgets(buf, sizeof(buf), fp)) {
                long long mb = atoll(buf);
                if (mb > 0)
                    info->total_ram_mb = (uint32_t)mb;
            }
            pclose(fp);
        }
        /* Fallback to /proc/meminfo if dmidecode gave nothing */
        if (info->total_ram_mb == 0) {
            fp = fopen("/proc/meminfo", "r");
            if (fp) {
                char line[256];
                while (fgets(line, sizeof(line), fp)) {
                    if (strncmp(line, "MemTotal:", 9) == 0) {
                        long long kb = atoll(line + 9);
                        info->total_ram_mb = (uint32_t)(
                            ((kb + 512*1024) / (1024*1024)) * 1024);
                        break;
                    }
                }
                fclose(fp);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  DIMM info (dmidecode)                                              */
/* ------------------------------------------------------------------ */

int collect_dimm_info(dimm_info_t *dimms, int max_dimms)
{
    int count = 0;

    FILE *fp = popen("dmidecode -t memory 2>/dev/null", "r");
    if (!fp) return 0;

    char line[512];
    int in_memdev = 0;
    char loc[64] = "", spd[32] = "", typ[32] = "";
    int size_mb_val = 0;

    while (fgets(line, sizeof(line), fp)) {
        int ll = strlen(line);
        while (ll > 0 && (line[ll-1]=='\n'||line[ll-1]=='\r'||line[ll-1]==' '))
            line[--ll] = '\0';

        /* dmidecode section: "Memory Device" at start of line (after optional whitespace) */
        if (strstr(line, "Memory Device") && !strstr(line, "Mapped")) {
            /* Save previous entry */
            if (in_memdev && size_mb_val > 0 && count < max_dimms) {
                dimm_info_t *d = &dimms[count];
                d->config_type = 0x00;
                d->ecc_type = 0x00;
                d->size_mb = (uint32_t)size_mb_val;
                strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                        sizeof(d->slot_name) - 1);
                strncpy(d->speed, spd[0] ? spd : "Unknown",
                        sizeof(d->speed) - 1);
                strncpy(d->type, typ[0] ? typ : "DDR3",
                        sizeof(d->type) - 1);
                count++;
            }
            in_memdev = 1;
            loc[0] = spd[0] = typ[0] = '\0';
            size_mb_val = 0;
            continue;
        }

        /* End section on "Handle" line (new dmidecode record) */
        if (strncmp(line, "Handle ", 7) == 0) {
            if (in_memdev && size_mb_val > 0 && count < max_dimms) {
                dimm_info_t *d = &dimms[count];
                d->config_type = 0x00;
                d->ecc_type = 0x00;
                d->size_mb = (uint32_t)size_mb_val;
                strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                        sizeof(d->slot_name) - 1);
                strncpy(d->speed, spd[0] ? spd : "Unknown",
                        sizeof(d->speed) - 1);
                strncpy(d->type, typ[0] ? typ : "DDR3",
                        sizeof(d->type) - 1);
                count++;
            }
            in_memdev = 0;
            loc[0] = spd[0] = typ[0] = '\0';
            size_mb_val = 0;
            continue;
        }

        if (!in_memdev) continue;

        /* Parse fields — dmidecode uses tab-indented "Key: Value" */
        char *p;
        while (line[0] == '\t' || line[0] == ' ') memmove(line, line+1, strlen(line));

        if ((p = strstr(line, "Locator:")) != NULL &&
            !strstr(line, "Bank")) {
            p += 8;
            while (*p == ' ') p++;
            strncpy(loc, p, sizeof(loc) - 1);
        }
        else if (strncmp(line, "Size:", 5) == 0) {
            p = line + 5;
            while (*p == ' ') p++;
            if (strstr(p, "No Module") || strstr(p, "Not")) {
                size_mb_val = 0;
            } else {
                size_mb_val = atoi(p);
                if (strstr(p, "GB")) size_mb_val *= 1024;
                else if (strstr(p, "KB")) size_mb_val /= 1024;
            }
        }
        else if (strncmp(line, "Type:", 5) == 0 &&
                 !strstr(line, "Type Detail") && !strstr(line, "Error")) {
            p = line + 5;
            while (*p == ' ') p++;
            if (strcmp(p, "Unknown") != 0)
                strncpy(typ, p, sizeof(typ) - 1);
        }
        else if (strncmp(line, "Speed:", 6) == 0 &&
                 !strstr(line, "Configured")) {
            p = line + 6;
            while (*p == ' ') p++;
            int mhz = atoi(p);
            if (mhz > 0)
                snprintf(spd, sizeof(spd), "%d MHz", mhz);
        }
    }

    /* Save last entry */
    if (in_memdev && size_mb_val > 0 && count < max_dimms) {
        dimm_info_t *d = &dimms[count];
        d->config_type = 0x00;
        d->ecc_type = 0x00;
        d->size_mb = (uint32_t)size_mb_val;
        strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                sizeof(d->slot_name) - 1);
        strncpy(d->speed, spd[0] ? spd : "Unknown",
                sizeof(d->speed) - 1);
        strncpy(d->type, typ[0] ? typ : "DDR3",
                sizeof(d->type) - 1);
        count++;
    }

    pclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Drive info (lsblk + /sys/block)                                    */
/* ------------------------------------------------------------------ */

int collect_drive_info(drive_info_t *drives, int max_drives)
{
    int count = 0;

    /* Scan /sys/block/ directly — more reliable than parsing lsblk */
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_drives) {
        char *name = ent->d_name;
        char sysname[128];

        /* Only real block devices: sd*, nvme*, hd* */
        if (strncmp(name, "sd", 2) != 0 &&
            strncmp(name, "nvme", 4) != 0 &&
            strncmp(name, "hd", 2) != 0)
            continue;

        /* Skip partitions (nvme0n1p1, sda1) */
        {
            int nlen = strlen(name);
            if (nlen > 0 && name[nlen-1] >= '0' && name[nlen-1] <= '9' &&
                strstr(name, "n1p"))
                continue;
        }

        drive_info_t *d = &drives[count];
        memset(d, 0, sizeof(*d));

        /* Size from /sys/block/DEV/size (in 512-byte sectors) */
        snprintf(sysname, sizeof(sysname), "/sys/block/%s/size", name);
        {
            char sbuf[32];
            if (read_file_trimmed(sysname, sbuf, sizeof(sbuf)) > 0) {
                long long sectors = atoll(sbuf);
                d->capacity_mb = (uint32_t)(sectors / 2048); /* sectors/2048 = MB */
            }
        }

        if (d->capacity_mb == 0) continue;  /* Skip zero-size devices */

        /* Model — NVMe uses a different sysfs path */
        if (strncmp(name, "nvme", 4) == 0) {
            snprintf(sysname, sizeof(sysname), "/sys/block/%s/device/model", name);
            read_file_trimmed(sysname, d->model, sizeof(d->model));
        } else {
            snprintf(sysname, sizeof(sysname), "/sys/block/%s/device/model", name);
            read_file_trimmed(sysname, d->model, sizeof(d->model));
        }

        /* Vendor */
        snprintf(sysname, sizeof(sysname), "/sys/block/%s/device/vendor", name);
        read_file_trimmed(sysname, d->vendor, sizeof(d->vendor));
        {
            int vl = strlen(d->vendor);
            while (vl > 0 && d->vendor[vl-1] == ' ') d->vendor[--vl] = '\0';
            if (strcmp(d->vendor, "ATA") == 0) d->vendor[0] = '\0';
        }
        if (!d->vendor[0] && d->model[0]) {
            char *sp = strchr(d->model, ' ');
            if (sp && (sp - d->model) >= 2 && (sp - d->model) < 20) {
                strncpy(d->vendor, d->model, sp - d->model);
                d->vendor[sp - d->model] = '\0';
            }
        }

        /* SSD/HDD — NVMe is always SSD, others check rotational flag */
        if (strncmp(name, "nvme", 4) == 0) {
            strncpy(d->kind, "SSD", 7);
        } else {
            int rota = 1;
            snprintf(sysname, sizeof(sysname), "/sys/block/%s/queue/rotational", name);
            char rbuf[8];
            if (read_file_trimmed(sysname, rbuf, sizeof(rbuf)) > 0)
                rota = atoi(rbuf);
            strncpy(d->kind, rota ? "HDD" : "SSD", 7);
        }

        /* Transport */
        if (strncmp(name, "nvme", 4) == 0)
            strncpy(d->iface, "NVMe", 15);
        else {
            snprintf(sysname, sizeof(sysname),
                     "/sys/block/%s/device/transport", name);
            char tbuf[16] = "";
            read_file_trimmed(sysname, tbuf, sizeof(tbuf));
            if (strstr(tbuf, "sas"))
                strncpy(d->iface, "SAS", 15);
            else if (strstr(tbuf, "sata") || strstr(tbuf, "ata"))
                strncpy(d->iface, "SATA", 15);
            else
                strncpy(d->iface, "SATA", 15);
        }

        snprintf(d->location, sizeof(d->location), "Bay %d", count + 1);
        int gb = (int)(d->capacity_mb / 1024);
        snprintf(d->display, sizeof(d->display), "%s %dG %s",
                 d->model, gb, d->kind);
        count++;
    }
    closedir(dir);
    return count;
}

/* ------------------------------------------------------------------ */
/*  NIC info (/sys/class/net + ip)                                     */
/* ------------------------------------------------------------------ */

int collect_nic_static(nic_static_t *nics, int max_nics)
{
    int count = 0;
    DIR *dir = opendir("/sys/class/net");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_nics) {
        char *name = ent->d_name;

        /* Skip virtual interfaces */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
            strcmp(name, "lo") == 0 ||
            strncmp(name, "veth", 4) == 0 ||
            strncmp(name, "br-", 3) == 0 ||
            strncmp(name, "docker", 6) == 0 ||
            strncmp(name, "vmbr", 4) == 0 ||
            strncmp(name, "tap", 3) == 0 ||
            strncmp(name, "bond", 4) == 0 ||
            strncmp(name, "fwbr", 4) == 0 ||
            strncmp(name, "fwpr", 4) == 0 ||
            strncmp(name, "fwln", 4) == 0)
            continue;

        /* Check it's a physical device (has /sys/class/net/DEV/device/) */
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/sys/class/net/%s/device", name);
        struct stat st;
        if (stat(devpath, &st) != 0) continue;

        strncpy(nics[count].name, name, 15);

        /* MAC address */
        char macpath[128];
        snprintf(macpath, sizeof(macpath), "/sys/class/net/%s/address", name);
        read_file_trimmed(macpath, nics[count].mac, sizeof(nics[count].mac));

        /* Driver */
        char drvpath[128], drvlink[256];
        snprintf(drvpath, sizeof(drvpath), "/sys/class/net/%s/device/driver", name);
        int len = readlink(drvpath, drvlink, sizeof(drvlink) - 1);
        if (len > 0) {
            drvlink[len] = '\0';
            char *base = strrchr(drvlink, '/');
            strncpy(nics[count].driver, base ? base + 1 : drvlink, 31);
        }

        count++;
    }
    closedir(dir);
    return count;
}

int collect_nic_dynamic(nic_dynamic_t *dynamic, const nic_static_t *nics_static,
                        int nic_count)
{
    for (int i = 0; i < nic_count; i++) {
        memset(&dynamic[i], 0, sizeof(dynamic[i]));
        const char *name = nics_static[i].name;
        char path[128], buf[32];

        /* Link state */
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
        read_file_trimmed(path, buf, sizeof(buf));
        strncpy(dynamic[i].link,
                (strcmp(buf, "up") == 0) ? "active" : "inactive",
                sizeof(dynamic[i].link) - 1);

        /* Speed */
        snprintf(path, sizeof(path), "/sys/class/net/%s/speed", name);
        if (read_file_trimmed(path, buf, sizeof(buf)) > 0 && atoi(buf) > 0)
            strncpy(dynamic[i].mbps, buf, sizeof(dynamic[i].mbps) - 1);

        /* Duplex */
        snprintf(path, sizeof(path), "/sys/class/net/%s/duplex", name);
        if (read_file_trimmed(path, buf, sizeof(buf)) > 0)
            strncpy(dynamic[i].duplex, buf, sizeof(dynamic[i].duplex) - 1);

        /* IP address — trace up through bond/bridge hierarchy.
         * Physical NIC may not have an IP directly; the IP could be on:
         *   NIC → bond → bridge → IP (Proxmox default)
         *   NIC → bridge → IP
         *   NIC → IP (simple setup) */
        {
            /* Try the NIC itself, then its master (bond), then bond's master (bridge) */
            char try_ifaces[3][32];
            int try_count = 0;
            char master_path[128], master_link[256];

            strncpy(try_ifaces[try_count++], name, 31);

            /* Check for bond master: /sys/class/net/NIC/master → bond */
            snprintf(master_path, sizeof(master_path),
                     "/sys/class/net/%s/master", name);
            int mlen = readlink(master_path, master_link, sizeof(master_link) - 1);
            if (mlen > 0) {
                master_link[mlen] = '\0';
                char *bond = strrchr(master_link, '/');
                if (bond) {
                    strncpy(try_ifaces[try_count++], bond + 1, 31);

                    /* Check for bridge master: /sys/class/net/BOND/master → bridge */
                    snprintf(master_path, sizeof(master_path),
                             "/sys/class/net/%s/master", bond + 1);
                    mlen = readlink(master_path, master_link, sizeof(master_link) - 1);
                    if (mlen > 0) {
                        master_link[mlen] = '\0';
                        char *br = strrchr(master_link, '/');
                        if (br) strncpy(try_ifaces[try_count++], br + 1, 31);
                    }
                }
            }

            /* Try each interface for an IP (most specific first, then up) */
            for (int t = try_count - 1; t >= 0 && !dynamic[i].ipv4[0]; t--) {
                char cmd[128];
                snprintf(cmd, sizeof(cmd),
                         "ip -4 addr show %s 2>/dev/null | grep inet",
                         try_ifaces[t]);
                FILE *fp = popen(cmd, "r");
                if (fp) {
                    char ipline[256];
                    if (fgets(ipline, sizeof(ipline), fp)) {
                        char *p = strstr(ipline, "inet ");
                        if (p) {
                            p += 5;
                            char *slash = strchr(p, '/');
                            if (slash) {
                                int iplen = slash - p;
                                if (iplen < (int)sizeof(dynamic[i].ipv4))
                                    strncpy(dynamic[i].ipv4, p, iplen);
                                int prefix = atoi(slash + 1);
                                uint32_t mask = prefix ? (~0U << (32 - prefix)) : 0;
                                snprintf(dynamic[i].netmask,
                                         sizeof(dynamic[i].netmask),
                                         "%d.%d.%d.%d",
                                         (mask >> 24) & 0xFF,
                                         (mask >> 16) & 0xFF,
                                         (mask >> 8) & 0xFF,
                                         mask & 0xFF);
                            }
                        }
                    }
                    pclose(fp);
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Change detection                                                   */
/* ------------------------------------------------------------------ */

static int cached_drive_count = -1;
static time_t cached_network_mtime = 0;

int detect_drive_changes(void)
{
    int count = 0;
    DIR *dir = opendir("/sys/block");
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "sd", 2) == 0 ||
            strncmp(ent->d_name, "nvme", 4) == 0 ||
            strncmp(ent->d_name, "hd", 2) == 0)
            count++;
    }
    closedir(dir);

    if (cached_drive_count < 0) {
        cached_drive_count = count;
        return 0;
    }
    if (count != cached_drive_count) {
        cached_drive_count = count;
        return 1;
    }
    return 0;
}

int detect_network_changes(void)
{
    struct stat st;
    /* Check multiple possible network config files */
    const char *paths[] = {
        "/etc/network/interfaces",
        "/etc/netplan",
        "/etc/sysconfig/network-scripts",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        if (stat(paths[i], &st) == 0) {
            if (cached_network_mtime == 0) {
                cached_network_mtime = st.st_mtime;
                return 0;
            }
            if (st.st_mtime != cached_network_mtime) {
                cached_network_mtime = st.st_mtime;
                return 1;
            }
            return 0;
        }
    }
    return -1;
}
