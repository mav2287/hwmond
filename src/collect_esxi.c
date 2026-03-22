/*
 * collect_esxi.c — ESXi-specific data collection for hwmond
 *
 * Collects system information from ESXi via esxcli, vsish, and smbiosDump.
 * Implements the platform-independent interface defined in collect.h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "collect.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int popen_field(const char *cmd, const char *field,
                       char *buf, int bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[512];
    buf[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, field);
        if (p) {
            p += strlen(field);
            while (*p == ' ' || *p == ':') p++;
            int len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'
                               || p[len-1] == ' '))
                p[--len] = '\0';
            strncpy(buf, p, bufsz - 1);
            buf[bufsz - 1] = '\0';
            pclose(fp);
            return strlen(buf);
        }
    }
    pclose(fp);
    return -1;
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

static void collapse_spaces(char *str)
{
    char *src = str, *dst = str;
    int prev_space = 1;
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
    if (dst > str && *(dst-1) == ' ') dst--;
    *dst = '\0';
}

/* ------------------------------------------------------------------ */
/*  System info                                                        */
/* ------------------------------------------------------------------ */

int collect_system_info(system_info_t *info)
{
    char buf[256];
    memset(info, 0, sizeof(*info));

    /* Firmware */
    if (popen_field("vsish -e get /hardware/bios/biosInfo",
                    "BIOS Version", buf, sizeof(buf)) > 0) {
        char *fw = buf; while (*fw == ' ') fw++;
        strncpy(info->firmware, fw, CACHE_SIZE - 1);
    }

    /* Hostname */
    popen_field("/bin/esxcli system hostname get", "Host Name",
                info->hostname, CACHE_SIZE);

    /* FQDN */
    popen_field("/bin/esxcli system hostname get",
                "Fully Qualified Domain Name", info->fqdn, CACHE_SIZE);

    /* OS info */
    popen_field("/bin/esxcli system version get", "Product",
                info->os_product, CACHE_SIZE);
    popen_field("/bin/esxcli system version get", "Version",
                info->os_version, CACHE_SIZE);
    {
        char update[32] = "", patch[32] = "", build[64] = "";
        popen_field("/bin/esxcli system version get", "Update",
                    update, sizeof(update));
        popen_field("/bin/esxcli system version get", "Patch",
                    patch, sizeof(patch));
        popen_field("/bin/esxcli system version get", "Build",
                    build, sizeof(build));
        snprintf(info->os_update, CACHE_SIZE, "Update %s", update);
        snprintf(info->os_build, CACHE_SIZE, "Build %s", build);
    }

    /* CPU */
    {
        popen_field("/bin/esxcli hardware cpu global get",
                    "CPU Packages", buf, sizeof(buf));
        info->cpu_packages = atoi(buf);
        popen_field("/bin/esxcli hardware cpu global get",
                    "CPU Cores", buf, sizeof(buf));
        info->cpu_cores = atoi(buf);
        popen_field("/bin/esxcli hardware cpu list",
                    "Core Speed", buf, sizeof(buf));
        info->cpu_speed_mhz = (int)((atol(buf) + 5000000) / 10000000) * 10;

        char cpu_model[128];
        popen_line("vsish -e get /hardware/cpu/cpuModelName",
                   cpu_model, sizeof(cpu_model));
        char *m = cpu_model; while (*m == ' ') m++;
        collapse_spaces(m);
        /* Strip "@ X.XX GHz" suffix — SM shows speed from binary data */
        char *at = strstr(m, " @");
        if (!at) at = strstr(m, " @ ");
        if (at) *at = '\0';
        int ml = strlen(m);
        while (ml > 0 && m[ml-1] == ' ') m[--ml] = '\0';
        strncpy(info->cpu_model, m, CACHE_SIZE - 1);
    }

    /* Serial / Model */
    popen_field("/bin/esxcli hardware platform get",
                "Serial Number", info->serial, CACHE_SIZE);
    popen_field("/bin/esxcli hardware platform get",
                "Product Name", info->model, CACHE_SIZE);

    /* Total RAM */
    popen_field("/bin/esxcli hardware memory get",
                "Physical Memory", buf, sizeof(buf));
    long long total_bytes = atoll(buf);
    info->total_ram_mb = (uint32_t)(
        ((total_bytes + (512LL * 1024 * 1024)) /
         (1024LL * 1024 * 1024)) * 1024);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  DIMM info (smbiosDump)                                             */
/* ------------------------------------------------------------------ */

int collect_dimm_info(dimm_info_t *dimms, int max_dimms)
{
    int count = 0;

    FILE *fp = popen("smbiosDump 2>/dev/null", "r");
    if (!fp) return 0;

    char line[512];
    int in_memdev = 0;
    char loc[64] = "", spd[32] = "", typ[32] = "";
    int size_mb_val = 0;

    while (fgets(line, sizeof(line), fp)) {
        int ll = strlen(line);
        while (ll > 0 && (line[ll-1]=='\n'||line[ll-1]=='\r'
                          ||line[ll-1]==' '))
            line[--ll] = '\0';

        /* Section header: 2-space indent + ": #N" */
        if (ll > 4 && line[0] == ' ' && line[1] == ' ' &&
            line[2] != ' ' && strstr(line, ": #")) {
            if (in_memdev && size_mb_val > 0 && count < max_dimms) {
                dimm_info_t *d = &dimms[count];
                d->config_type = 0x00;
                d->ecc_type = 0x00;
                d->size_mb = (uint32_t)size_mb_val;
                strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                        sizeof(d->slot_name) - 1);
                strncpy(d->speed, spd[0] ? spd : "Unknown",
                        sizeof(d->speed) - 1);
                strncpy(d->type, typ[0] ? typ : "DDR3 ECC",
                        sizeof(d->type) - 1);
                count++;
            }
            in_memdev = (strstr(line, "Memory Device") != NULL &&
                         strstr(line, "Mapped") == NULL) ? 1 : 0;
            loc[0] = spd[0] = typ[0] = '\0';
            size_mb_val = 0;
            continue;
        }

        if (!in_memdev) continue;
        if (line[0] != ' ' || line[1] != ' ' ||
            line[2] != ' ' || line[3] != ' ')
            continue;

        char *field = line + 4;
        char *p;
        int l;

        if ((p = strstr(field, "Location:")) != NULL && (p += 9)) {
            while (*p == ' ') p++;
            if (*p == '"') p++;
            l = strlen(p);
            if (l > 0 && p[l-1] == '"') p[--l] = '\0';
            strncpy(loc, p, sizeof(loc) - 1);
        }
        else if (strncmp(field, "Size:", 5) == 0 && !strstr(field, "Max")) {
            p = field + 5;
            while (*p == ' ') p++;
            if (strstr(p, "No Memory") || strstr(p, "Not")) {
                size_mb_val = 0;
            } else {
                size_mb_val = atoi(p);
                if (strstr(p, "GB")) size_mb_val *= 1024;
                else if (strstr(p, "KB")) size_mb_val /= 1024;
            }
        }
        else if (((p = strstr(field, "Memory Type:")) != NULL && (p += 12)) ||
                 ((p = strstr(field, "Type:")) != NULL &&
                  !strstr(field, "Type Detail") && !strstr(field, "Error") &&
                  (p += 5))) {
            while (*p == ' ') p++;
            char *paren = strchr(p, '(');
            if (paren) {
                paren++;
                char *end = strchr(paren, ')');
                if (end) {
                    l = end - paren;
                    if (l > (int)sizeof(typ) - 1) l = sizeof(typ) - 1;
                    strncpy(typ, paren, l);
                    typ[l] = '\0';
                    if (strcmp(typ, "Unknown") == 0) typ[0] = '\0';
                }
            }
        }
        else if (strncmp(field, "Speed:", 6) == 0) {
            p = field + 6;
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
        strncpy(d->type, typ[0] ? typ : "DDR3 ECC",
                sizeof(d->type) - 1);
        count++;
    }

    pclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Drive info (esxcli)                                                */
/* ------------------------------------------------------------------ */

int collect_drive_info(drive_info_t *drives, int max_drives)
{
    int count = 0;

    FILE *fp = popen("/bin/esxcli storage core device list 2>/dev/null", "r");
    if (!fp) return 0;

    char line[512];
    char dev_id[256] = "", model[64] = "", size_str[32] = "";
    char vendor[32] = "", is_ssd[8] = "", is_sas[8] = "";
    char dev_type[32] = "", display_name[128] = "";
    int have_device = 0;

    while (1) {
        int got_line = (fgets(line, sizeof(line), fp) != NULL);
        char *p;
        int l;

        int new_device = (!got_line) ||
            (line[0] != ' ' && line[0] != '\n' && line[0] != '-' &&
             strlen(line) > 3);

        if (new_device && have_device) {
            if (model[0] && atol(size_str) > 0 &&
                strstr(dev_type, "Direct") && count < max_drives) {
                long mb = atol(size_str);
                drive_info_t *d = &drives[count];
                memset(d, 0, sizeof(*d));

                d->capacity_mb = (uint32_t)mb;
                d->reserved = 0;
                strncpy(d->kind, strstr(is_ssd, "true") ? "SSD" : "HDD", 7);
                strncpy(d->model, model, 31);

                /* Manufacturer from multiple sources */
                if (vendor[0] && strcmp(vendor, "ATA") != 0 &&
                    strcmp(vendor, "NVMe") != 0)
                    strncpy(d->vendor, vendor, 31);
                if (!d->vendor[0] && strncmp(dev_id, "t10.", 4) == 0) {
                    char *s = dev_id + 4;
                    while (*s && *s != '_') s++;
                    while (*s == '_') s++;
                    if (*s) {
                        char *e = s;
                        while (*e && *e != '_' && *e != ' ') e++;
                        int wl = e - s;
                        if (wl >= 2 && wl < 30) {
                            strncpy(d->vendor, s, wl);
                            d->vendor[wl] = '\0';
                        }
                    }
                }
                if (!d->vendor[0] && display_name[0] &&
                    strncmp(display_name, "Local", 5) != 0) {
                    char *sp = strchr(display_name, ' ');
                    if (sp && (sp - display_name) >= 2 && (sp - display_name) < 30) {
                        strncpy(d->vendor, display_name, sp - display_name);
                        d->vendor[sp - display_name] = '\0';
                    }
                }
                if (!d->vendor[0]) {
                    char *sp = strchr(model, ' ');
                    if (sp && (sp - model) >= 2 && (sp - model) < 20) {
                        strncpy(d->vendor, model, sp - model);
                        d->vendor[sp - model] = '\0';
                    }
                }

                /* Interconnect */
                if (strstr(dev_id, "NVMe") || strstr(dev_id, "nvme"))
                    strncpy(d->iface, "NVMe", 15);
                else if (strstr(display_name, "Fibre Channel"))
                    strncpy(d->iface, "Fibre Channel", 15);
                else if (strstr(is_sas, "true"))
                    strncpy(d->iface, "SAS", 15);
                else
                    strncpy(d->iface, "SATA", 15);

                snprintf(d->location, sizeof(d->location), "Bay %d", count + 1);

                int gb = (int)(mb / 1024);
                snprintf(d->display, sizeof(d->display), "%s %dG %s",
                         model, gb, d->kind);
                count++;
            }
            dev_id[0] = model[0] = size_str[0] = '\0';
            vendor[0] = is_ssd[0] = is_sas[0] = '\0';
            dev_type[0] = display_name[0] = '\0';
            have_device = 0;
        }

        if (!got_line) break;

        if (line[0] != ' ' && line[0] != '\n' && line[0] != '-') {
            l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == ' '))
                line[--l] = '\0';
            strncpy(dev_id, line, sizeof(dev_id) - 1);
            have_device = 1;
        } else if ((p = strstr(line, "Display Name:")) != NULL &&
                   !strstr(line, "Settable")) {
            p += 13; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(display_name, p, sizeof(display_name) - 1);
        } else if ((p = strstr(line, "Model:")) != NULL &&
                   !strstr(line, "Multipath")) {
            p += 6; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(model, p, sizeof(model) - 1);
        } else if ((p = strstr(line, "Vendor:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(vendor, p, sizeof(vendor) - 1);
        } else if ((p = strstr(line, "Device Type:")) != NULL) {
            p += 12; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(dev_type, p, sizeof(dev_type) - 1);
        } else if (strstr(line, "  Size:") && !strstr(line, "Queue") &&
                   !strstr(line, "Cache") && !strstr(line, "Sample")) {
            p = strstr(line, "Size:") + 5;
            while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(size_str, p, sizeof(size_str) - 1);
        } else if ((p = strstr(line, "Is SAS:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            strncpy(is_sas, p, 5); is_sas[5] = '\0';
        } else if ((p = strstr(line, "Is SSD:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            strncpy(is_ssd, p, 5); is_ssd[5] = '\0';
        }
    }
    pclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/*  NIC info (esxcli)                                                  */
/* ------------------------------------------------------------------ */

int collect_nic_static(nic_static_t *nics, int max_nics)
{
    int count = 0;
    FILE *fp = popen("/bin/esxcli network nic list 2>/dev/null", "r");
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char name[16], pci[16], driver[16], admin[8], link[8];
        char duplex[8], mac[24];
        int speed;
        if (sscanf(line, "%15s %15s %15s %7s %7s %d %7s %23s",
                   name, pci, driver, admin, link, &speed, duplex, mac) >= 8
            && strncmp(name, "vmnic", 5) == 0
            && count < max_nics) {
            strncpy(nics[count].name, name, 15);
            strncpy(nics[count].mac, mac, 23);
            strncpy(nics[count].driver, driver, 31);
            count++;
        }
    }
    pclose(fp);
    return count;
}

int collect_nic_dynamic(nic_dynamic_t *dynamic, const nic_static_t *nics_static, int nic_count)
{
    /* Link state, speed, duplex from NIC list */
    FILE *fp = popen("/bin/esxcli network nic list 2>/dev/null", "r");
    if (!fp) return -1;

    char line[512];
    int idx = 0;
    while (fgets(line, sizeof(line), fp)) {
        char name[16], pci[16], driver[16], admin[8], link[8];
        char duplex_str[8], mac[24];
        int speed;
        if (sscanf(line, "%15s %15s %15s %7s %7s %d %7s %23s",
                   name, pci, driver, admin, link, &speed, duplex_str, mac) >= 8
            && strncmp(name, "vmnic", 5) == 0
            && idx < nic_count) {
            memset(&dynamic[idx], 0, sizeof(dynamic[idx]));
            strncpy(dynamic[idx].link,
                    (strcmp(link, "Up") == 0) ? "active" : "inactive",
                    sizeof(dynamic[idx].link) - 1);
            snprintf(dynamic[idx].mbps, sizeof(dynamic[idx].mbps), "%d", speed);
            strncpy(dynamic[idx].duplex, duplex_str, 7);
            idx++;
        }
    }
    pclose(fp);

    /* Get IP addresses */
    fp = popen("/bin/esxcli network ip interface ipv4 get 2>/dev/null", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char iface[16], ip[20], mask[20];
            if (sscanf(line, "%15s %19s %19s", iface, ip, mask) >= 3) {
                if (strncmp(iface, "vmk", 3) == 0) {
                    int vmk_idx = atoi(iface + 3);
                    if (vmk_idx >= 0 && vmk_idx < nic_count) {
                        strncpy(dynamic[vmk_idx].ipv4, ip, 19);
                        strncpy(dynamic[vmk_idx].netmask, mask, 19);
                    }
                }
            }
        }
        pclose(fp);
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
    FILE *fp = popen("ls /vmfs/devices/disks/ 2>/dev/null | wc -l", "r");
    if (!fp) return -1;
    char buf[32];
    int count = 0;
    if (fgets(buf, sizeof(buf), fp))
        count = atoi(buf);
    pclose(fp);

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
    if (stat("/etc/vmware/esx.conf", &st) != 0)
        return -1;

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
