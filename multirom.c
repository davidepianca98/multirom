#include <sys/stat.h> 
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h> 
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/klog.h>
#include <linux/loop.h>

#include "multirom.h"
#include "multirom_ui.h"
#include "framebuffer.h"
#include "input.h"
#include "log.h"
#include "util.h"

#define REALDATA "/realdata"
#define BUSYBOX_BIN "busybox"
#define KEXEC_BIN "kexec"
#define INTERNAL_ROM_NAME "Internal"
#define BOOT_BLK "/dev/block/mmcblk0p2"
#define IN_ROOT "is_in_root"
#define MAX_ROM_NAME_LEN 26
#define LAYOUT_VERSION "/data/.layout_version"

#define T_FOLDER 4

static char multirom_dir[64] = { 0 };
static char busybox_path[128] = { 0 };
static char kexec_path[128] = { 0 };
static volatile int run_usb_refresh = 0;
static pthread_t usb_refresh_thread;
static pthread_mutex_t parts_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*usb_refresh_handler)(void) = NULL;

int multirom_find_base_dir(void)
{
    int i;
    struct stat info;

    static const char *paths[] = {
        REALDATA"/media/0/multirom", // 4.2
        REALDATA"/media/multirom",
        NULL,
    };

    for(i = 0; paths[i]; ++i)
    {
        if(stat(paths[i], &info) < 0)
            continue;

        strcpy(multirom_dir, paths[i]);
        sprintf(busybox_path, "%s/%s", paths[i], BUSYBOX_BIN);
        sprintf(kexec_path, "%s/%s", paths[i], KEXEC_BIN);
        return 0;
    }
    return -1;
}

int multirom(void)
{
    if(multirom_find_base_dir() == -1)
    {
        ERROR("Could not find multirom dir");
        return -1;
    }

    struct multirom_status s;
    memset(&s, 0, sizeof(struct multirom_status));

    multirom_load_status(&s);
    multirom_dump_status(&s);

    struct multirom_rom *to_boot = NULL;
    int exit = (EXIT_REBOOT | EXIT_UMOUNT);

    if(s.is_second_boot == 0)
    {
        // just to cache the result so that it does not take
        // any time when the UI is up
        multirom_has_kexec();

        switch(multirom_ui(&s, &to_boot))
        {
            case UI_EXIT_BOOT_ROM: break;
            case UI_EXIT_REBOOT:
                exit = (EXIT_REBOOT | EXIT_UMOUNT);
                break;
            case UI_EXIT_REBOOT_RECOVERY:
                exit = (EXIT_REBOOT_RECOVERY | EXIT_UMOUNT);
                break;
            case UI_EXIT_REBOOT_BOOTLOADER:
                exit = (EXIT_REBOOT_BOOTLOADER | EXIT_UMOUNT);
                break;
            case UI_EXIT_SHUTDOWN:
                exit = (EXIT_SHUTDOWN | EXIT_UMOUNT);
                break;
        }
    }
    else
    {
        ERROR("Skipping ROM selection beacause of is_second_boot==1");
        to_boot = s.current_rom;
    }

    if(to_boot)
    {
        exit = multirom_prepare_for_boot(&s, to_boot);

        // Something went wrong, reboot
        if(exit == -1)
        {
            multirom_emergency_reboot();
            return EXIT_REBOOT;
        }

        s.current_rom = to_boot;
        if(s.is_second_boot == 0 && (M(to_boot->type) & MASK_ANDROID) && (exit & EXIT_KEXEC))
            s.is_second_boot = 1;
        else
            s.is_second_boot = 0;
    }

    multirom_save_status(&s);
    multirom_free_status(&s);

    sync();

    return exit;
}

void multirom_emergency_reboot(void)
{
    if(multirom_init_fb() < 0)
    {
        ERROR("Failed to init framebuffer in emergency reboot");
        return;
    }

    fb_add_text(0, 150, WHITE, SIZE_NORMAL, 
                "An error occured.\nShutting down MultiROM to avoid data corruption.\n"
                "Report this error to the developer!\nDebug info: /sdcard/multirom/error.txt\n\n"
                "Press POWER button to reboot.");

    fb_draw();
    fb_clear();
    fb_close();

    // dump klog
    int len = klogctl(10, NULL, 0);
    if      (len < 16*1024)      len = 16*1024;
    else if (len > 16*1024*1024) len = 16*1024*1024;

    char *buff = malloc(len);
    klogctl(3, buff, len);
    if(len > 0)
    {
        FILE *f = fopen(REALDATA"/media/multirom/error.txt", "w");
        if(f)
        {
            fwrite(buff, 1, len, f);
            fclose(f);
            chmod(REALDATA"/media/multirom/error.txt", 0777);
        }
    }
    free(buff);

    // Wait for power key
    start_input_thread();
    while(wait_for_key() != KEY_POWER);
    stop_input_thread();
}

static int compare_rom_names(const void *a, const void *b)
{
    struct multirom_rom *rom_a = *((struct multirom_rom **)a);
    struct multirom_rom *rom_b = *((struct multirom_rom **)b);
    return strcoll(rom_a->name, rom_b->name);
}

int multirom_default_status(struct multirom_status *s)
{
    s->is_second_boot = 0;
    s->current_rom = NULL;
    s->roms = NULL;

    char roms_path[256];
    sprintf(roms_path, "%s/roms/"INTERNAL_ROM_NAME, multirom_dir);
    DIR *d = opendir(roms_path);
    if(!d)
    {
        ERROR("Failed to open Internal ROM's folder, creating one with ROM from internal memory...\n");
        if(multirom_import_internal() == -1)
            return -1;
    }
    else
        closedir(d);

    sprintf(roms_path, "%s/roms", multirom_dir);
    d = opendir(roms_path);
    if(!d)
    {
        ERROR("Failed to open roms dir!\n");
        return -1;
    }

    struct dirent *dr;
    char path[256];
    struct multirom_rom **add_roms = NULL;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        if(dr->d_type != T_FOLDER)
            continue;

        if(strlen(dr->d_name) > MAX_ROM_NAME_LEN)
        {
            ERROR("Skipping ROM %s, name is too long (max %d chars allowed)", dr->d_name, MAX_ROM_NAME_LEN);
            continue;
        }

        fb_debug("Adding ROM %s\n", dr->d_name);

        struct multirom_rom *rom = malloc(sizeof(struct multirom_rom));
        memset(rom, 0, sizeof(struct multirom_rom));

        rom->id = multirom_generate_rom_id();
        rom->name = strdup(dr->d_name);

        sprintf(path, "%s/%s", roms_path, rom->name);
        rom->base_path = strdup(path);

        rom->type = multirom_get_rom_type(rom);

        sprintf(path, "%s/%s", rom->base_path, IN_ROOT);
        rom->is_in_root = access(path, R_OK) == 0 ? 1 : 0;

        sprintf(path, "%s/boot.img", rom->base_path, rom->name);
        rom->has_bootimg = access(path, R_OK) == 0 ? 1 : 0;

        list_add(rom, &add_roms);
    }

    closedir(d);

    if(add_roms)
    {
        // sort roms
        qsort(add_roms, list_item_count(add_roms), sizeof(struct multirom_rom*), compare_rom_names);

        //add them to main list
        int i;
        for(i = 0; add_roms[i]; ++i)
            list_add(add_roms[i], &s->roms);
        list_clear(&add_roms, NULL);
    }

    s->current_rom = multirom_get_rom(s, INTERNAL_ROM_NAME);
    if(!s->current_rom)
    {
        fb_debug("No internal rom found!\n");
        return -1;
    }
    return 0;
}

int multirom_load_status(struct multirom_status *s)
{
    fb_debug("Loading MultiROM status...\n");

    multirom_default_status(s);

    char arg[256];
    sprintf(arg, "%s/multirom.ini", multirom_dir);

    FILE *f = fopen(arg, "r");
    if(!f)
    {
        fb_debug("Failed to open config file, using defaults!\n");
        return -1;
    }

    char current_rom[256] = { 0 };
    char auto_boot_rom[256] = { 0 };

    char line[512];
    char name[64];
    char *pch;

    while((fgets(line, sizeof(line), f)))
    {
        pch = strtok (line, "=\n");
        if(!pch) continue;
        strcpy(name, pch);
        pch = strtok (NULL, "=\n");
        if(!pch) continue;
        strcpy(arg, pch);

        if(strstr(name, "is_second_boot"))
            s->is_second_boot = atoi(arg);
        else if(strstr(name, "current_rom"))
            strcpy(current_rom, arg);
        else if(strstr(name, "auto_boot_seconds"))
            s->auto_boot_seconds = atoi(arg);
        else if(strstr(name, "auto_boot_rom"))
            strcpy(auto_boot_rom, arg);
    }

    fclose(f);

    s->current_rom = multirom_get_rom(s, current_rom);
    if(!s->current_rom)
    {
        fb_debug("Failed to select current rom (%s), using Internal!\n", current_rom);
        s->current_rom = multirom_get_rom(s, INTERNAL_ROM_NAME);
        if(!s->current_rom)
        {
            fb_debug("No internal rom found!\n");
            return -1;
        }
    }

    s->auto_boot_rom = multirom_get_rom(s, auto_boot_rom);
    if(!s->auto_boot_rom)
        ERROR("Could not find rom %s to auto-boot", auto_boot_rom);

    return 0;
}

int multirom_save_status(struct multirom_status *s)
{
    fb_debug("Saving multirom status\n");

    char path[256];
    sprintf(path, "%s/multirom.ini", multirom_dir);

    FILE *f = fopen(path, "w");
    if(!f)
    {
        fb_debug("Failed to open/create status file!\n");
        return -1;
    }

    fprintf(f, "is_second_boot=%d\n", s->is_second_boot);
    fprintf(f, "current_rom=%s\n", s->current_rom ? s->current_rom->name : INTERNAL_ROM_NAME);
    fprintf(f, "auto_boot_seconds=%d\n", s->auto_boot_seconds);
    fprintf(f, "auto_boot_rom=%s\n", s->auto_boot_rom ? s->auto_boot_rom->name : "");

    fclose(f);
    return 0;
}

void multirom_find_usb_roms(struct multirom_status *s)
{
    // remove USB roms
    int i;
    for(i = 0; s->roms && s->roms[i];)
    {
        if(M(s->roms[i]->type) & MASK_USB_ROMS)
        {
            list_rm(s->roms[i], &s->roms, &multirom_free_rom);
            i = 0;
        }
        else ++i;
    }

    char path[256];
    struct usb_partition *p;
    struct stat info;
    DIR *d;
    struct dirent *dr;
    struct multirom_rom **add_roms = NULL;

    pthread_mutex_lock(&parts_mutex);
    for(i = 0; s->partitions && s->partitions[i]; ++i)
    {
        p = s->partitions[i];
 
        sprintf(path, "%s/multirom", p->mount_path);
        if(stat(path, &info) < 0)
            continue;

        d = opendir(path);
        if(!d)
            continue;

        while((dr = readdir(d)) != NULL)
        {
            if(dr->d_name[0] == '.')
                continue;

            struct multirom_rom *rom = malloc(sizeof(struct multirom_rom));
            memset(rom, 0, sizeof(struct multirom_rom));

            rom->id = multirom_generate_rom_id();
            rom->name = strdup(dr->d_name);

            sprintf(path, "%s/multirom/%s", p->mount_path, rom->name);
            rom->base_path = strdup(path);

            rom->partition = p;
            rom->type = multirom_get_rom_type(rom);

            sprintf(path, "%s/%s", rom->base_path, IN_ROOT);
            rom->is_in_root = access(path, R_OK) == 0 ? 1 : 0;

            sprintf(path, "%s/boot.img", rom->base_path);
            rom->has_bootimg = access(path, R_OK) == 0 ? 1 : 0;

            list_add(rom, &add_roms);
        }
        closedir(d);
    }
    pthread_mutex_unlock(&parts_mutex);

    if(add_roms)
    {
        // sort roms
        qsort(add_roms, list_item_count(add_roms), sizeof(struct multirom_rom*), compare_rom_names);

        //add them to main list
        for(i = 0; add_roms[i]; ++i)
            list_add(add_roms[i], &s->roms);
        list_clear(&add_roms, NULL);
    }

    multirom_dump_status(s);
}

int multirom_get_rom_type(struct multirom_rom *rom)
{
    if(!rom->partition && strcmp(rom->name, INTERNAL_ROM_NAME) == 0)
        return ROM_DEFAULT;

#define FOLDERS 3
    static const char *folders[][FOLDERS] = 
    {
         { "system", "data", "cache" },
         { "root", NULL, NULL },
         { "system.img", "data.img", "cache.img" },
         { "root.img", NULL, NULL }
    };
    static const int types_int[] = { ROM_ANDROID_INTERNAL, ROM_UBUNTU_INTERNAL, ROM_UNKNOWN, ROM_UNKNOWN };
    static const int types_usb[] = { ROM_ANDROID_USB_DIR, ROM_UBUNTU_USB_DIR, ROM_ANDROID_USB_IMG, ROM_UBUNTU_USB_IMG };

    char path[256];
    uint32_t i, y, okay;
    for(i = 0; i < ARRAY_SIZE(folders); ++i)
    {
        okay = 1;
        for(y = 0; folders[i][y] && y < FOLDERS && okay; ++y)
        {
            sprintf(path, "%s/%s", rom->base_path, folders[i][y]);
            if(access(path, R_OK) < 0)
                okay = 0;
        }

        if(okay)
        {
            if(!rom->partition)
                return types_int[i];
            else
                return types_usb[i];
        }
    }
    return ROM_UNKNOWN;
}

int multirom_import_internal(void)
{
    char path[256];

    // multirom
    mkdir(multirom_dir, 0777); 

    // roms
    sprintf(path, "%s/roms", multirom_dir);
    mkdir(path, 0777);

    // internal rom
    sprintf(path, "%s/roms/%s", multirom_dir, INTERNAL_ROM_NAME);
    mkdir(path, 0777);

    // boot image
    sprintf(path, "%s/roms/%s/boot.img", multirom_dir, INTERNAL_ROM_NAME);
    int res = multirom_dump_boot(path);

    // is_in_root file
    sprintf(path, "%s/roms/%s/%s", multirom_dir, INTERNAL_ROM_NAME, IN_ROOT);
    FILE *f = fopen(path, "w");
    if(f)
        fclose(f);
    return res;
}

int multirom_dump_boot(const char *dest)
{
    fb_debug("Dumping boot image...");

    //              0            1     2             3
    char *cmd[] = { busybox_path, "dd", "if="BOOT_BLK, NULL, NULL };
    cmd[3] = malloc(256);
    sprintf(cmd[3], "of=%s", dest);

    int res = run_cmd(cmd);
    free(cmd[3]);

    fb_debug("done, result: %d\n", res);
    return res;
}

struct multirom_rom *multirom_get_rom(struct multirom_status *s, const char *name)
{
    int i = 0;
    for(; s->roms && s->roms[i]; ++i)
        if(strcmp(s->roms[i]->name, name) == 0)
            return s->roms[i];

    return NULL;
}

struct multirom_rom *multirom_get_rom_in_root(struct multirom_status *s)
{
    int i = 0;
    for(; s->roms && s->roms[i]; ++i)
        if(s->roms[i]->is_in_root)
            return s->roms[i];

    return NULL;
}

int multirom_generate_rom_id(void)
{
    static int id = 0;
    return id++;
}

struct multirom_rom *multirom_get_rom_by_id(struct multirom_status *s, int id)
{
    int i = 0;
    for(; s->roms && s->roms[i]; ++i)
        if(s->roms[i]->id == id)
            return s->roms[i];
    return NULL;
}

void multirom_dump_status(struct multirom_status *s)
{
    fb_debug("Dumping multirom status:\n");
    fb_debug("  is_second_boot=%d\n", s->is_second_boot);
    fb_debug("  current_rom=%s\n", s->current_rom ? s->current_rom->name : "NULL");
    fb_debug("  auto_boot_seconds=%d\n", s->auto_boot_seconds);
    fb_debug("  auto_boot_rom=%s\n", s->auto_boot_rom ? s->auto_boot_rom->name : "NULL");
    fb_debug("\n");

    int i, y;
    char buff[256];
    for(i = 0; s->roms && s->roms[i]; ++i)
    {
        fb_debug("  ROM: %s\n", s->roms[i]->name);
        fb_debug("    base_path: %s\n", s->roms[i]->base_path);
        fb_debug("    type: %d\n", s->roms[i]->type);
        fb_debug("    is_in_root: %d\n", s->roms[i]->is_in_root);
        fb_debug("    has_bootimg: %d\n", s->roms[i]->has_bootimg);
    }
}

int multirom_prepare_for_boot(struct multirom_status *s, struct multirom_rom *to_boot)
{
    int exit = EXIT_UMOUNT;

    if(to_boot->has_bootimg && to_boot->type != ROM_DEFAULT && s->is_second_boot == 0)
    {
        if(multirom_load_kexec(to_boot) != 0)
            return -1;
        exit |= EXIT_KEXEC;
    }

    if(to_boot == s->current_rom)
        fb_debug("To-boot rom is the same as previous rom.\n");

    int type_now = s->current_rom->type;
    int type_to = to_boot->type;

    // move root if needed
    if (!to_boot->is_in_root &&
        (to_boot->type == ROM_UBUNTU_INTERNAL || to_boot->type == ROM_DEFAULT))
    {
        struct multirom_rom *in_root = multirom_get_rom_in_root(s);
        if(!in_root)
        {
            ERROR("No rom in root!");
            return -1;
        }

        if (multirom_move_out_of_root(in_root) == -1 ||
            multirom_move_to_root(to_boot) == -1)
            return -1;
    }

    switch(type_to)
    {
        case ROM_DEFAULT:
            break;
        case ROM_UBUNTU_INTERNAL:
        {
            struct stat info;
            if(!(exit & (EXIT_REBOOT | EXIT_KEXEC)) && stat("/init.rc", &info) >= 0)
            {
                ERROR("Trying to boot ubuntu with android boot.img, aborting!\n");
                return -1;
            }
            break;
        }
        case ROM_ANDROID_USB_IMG:
        case ROM_ANDROID_USB_DIR:
        case ROM_ANDROID_INTERNAL:
        {
            if(!(exit & (EXIT_REBOOT | EXIT_KEXEC)))
                exit &= ~(EXIT_UMOUNT);

            if(multirom_prep_android_mounts(to_boot) == -1)
                return -1;

            if(multirom_create_media_link() == -1)
                return -1;

            if(to_boot->partition)
                to_boot->partition->keep_mounted = 1;

            struct stat info;
            if(!(exit & (EXIT_REBOOT | EXIT_KEXEC)) && stat("/init.rc", &info) < 0)
            {
                ERROR("Trying to boot android with ubuntu boot.img, aborting!\n");
                return -1;
            }
            break;
        }
        default:
            ERROR("Unknown ROM type\n");
            return -1;
    }

    return exit;
}

int multirom_move_out_of_root(struct multirom_rom *rom)
{
    fb_debug("Moving ROM %s out of root...\n", rom->name);

    char path_to[256];
    sprintf(path_to, "%s/roms/%s/root/", multirom_dir, rom->name);

    mkdir(path_to, 0777);

    DIR *d = opendir(REALDATA);
    if(!d)
    {
        fb_debug("Failed to open /realdata!\n");
        return -1;
    }

    //              0           1     2            3
    char *cmd[] = { busybox_path, "mv", malloc(256), path_to, NULL };
    struct dirent *dr;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.' && (dr->d_name[1] == '.' || dr->d_name[1] == 0))
            continue;

        if(strcmp(dr->d_name, "media") == 0)
            continue;

        sprintf(cmd[2], REALDATA"/%s", dr->d_name);
        int res = run_cmd(cmd);
        if(res != 0)
        {
            fb_debug("Move failed %d\n%s\n%s\n%s\n%s\n", res, cmd[0], cmd[1], cmd[2], cmd[3]);
            free(cmd[2]);
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    free(cmd[2]);

    sprintf(path_to, "%s/roms/%s/%s", multirom_dir, rom->name, IN_ROOT);
    unlink(path_to);

    return 0;
}

int multirom_move_to_root(struct multirom_rom *rom)
{
    fb_debug("Moving ROM %s to root...\n", rom->name);

    char path_from[256];
    sprintf(path_from, "%s/roms/%s/root/", multirom_dir, rom->name);

    DIR *d = opendir(path_from);
    if(!d)
    {
        fb_debug("Failed to open %s!\n", path_from);
        return -1;
    }

    //              0           1     2            3
    char *cmd[] = { busybox_path, "mv", malloc(256), "/realdata/", NULL };
    struct dirent *dr;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.' && (dr->d_name[1] == '.' || dr->d_name[1] == 0))
            continue;

        if(strcmp(dr->d_name, "media") == 0)
            continue;

        sprintf(cmd[2], "%s%s", path_from, dr->d_name);
        int res = run_cmd(cmd);
        if(res != 0)
        {
            fb_debug("Move failed %d\n%s\n%s\n%s\n%s\n", res, cmd[0], cmd[1], cmd[2], cmd[3]);
            free(cmd[2]);
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    free(cmd[2]);
    sprintf(path_from, "%s/roms/%s/%s", multirom_dir, rom->name, IN_ROOT);
    FILE *f = fopen(path_from, "w");
    if(f)
        fclose(f);

    return 0;
}

void multirom_free_status(struct multirom_status *s)
{
    list_clear(&s->partitions, &multirom_destroy_partition);
    list_clear(&s->roms, &multirom_free_rom);
}

void multirom_free_rom(void *rom)
{
    free(((struct multirom_rom*)rom)->name);
    free(((struct multirom_rom*)rom)->base_path);
    free(rom);
}

int multirom_init_fb(void)
{
    vt_set_mode(1);

    if(fb_open() < 0)
    {
        ERROR("Failed to open framebuffer!");
        return -1;
    }

    fb_fill(BLACK);
    return 0;
}

#define EXEC_MASK (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP)

int multirom_prep_android_mounts(struct multirom_rom *rom)
{
    char in[128];
    char out[128];
    char folder[256];
    sprintf(folder, "%s/boot", rom->base_path);

    DIR *d = opendir(folder);
    if(!d)
    {
        ERROR("Failed to open rom folder %s", folder);
        return -1;
    }

    struct dirent *dp = NULL;

    char line[1024];
    FILE *f_in = NULL;
    FILE *f_out = NULL;
    int add_dummy = 0;

    while((dp = readdir(d)))
    {
        sprintf(in, "%s/%s", folder, dp->d_name);
        sprintf(out, "/%s", dp->d_name);

        // just copy the file if not rc
        if(!strstr(dp->d_name, ".rc"))
        {
            copy_file(in, out);
            continue;
        }

        f_in = fopen(in, "r");
        if(!f_in)
            continue;

        f_out = fopen(out, "w");
        if(!f_out)
        {
            fclose(f_in);
            continue;
        }

        while((fgets(line, sizeof(line), f_in)))
        {
            if (strstr(line, "on "))
                add_dummy = 1;
            // Remove mounts from RCs
            else if (strstr(line, "mount_all") || 
               (strstr(line, "mount ") && (strstr(line, "/data") || strstr(line, "/system"))))
            {
                if(add_dummy == 1)
                {
                    add_dummy = 0;
                    fputs("    export DUMMY_LINE_INGORE_IT 1\n", f_out);
                }
                fputc((int)'#', f_out);
            }
            // fixup sdcard tool
            else if(strstr(line, "service sdcard") == line)
            {
                // not needed, now I use bind insead of link
                // so just put the line as is
                fputs(line, f_out);
                /*char *p = strtok(line, " ");
                while(p)
                {
                    if(strcmp(p, "/data/media") == 0)
                        fputs("/realdata/media", f_out);
                    else
                        fputs(p, f_out);

                    if((p = strtok(NULL, " ")))
                        fputc((int)' ', f_out);
                }*/

                // put it to main class and skip next line,
                // it does not start on CM10 when it is in late_start, wtf?
                fputs("    class main\n", f_out);
                fgets(line, sizeof(line), f_in); // skip next line, should be "class late_start"
                continue;
            }
            fputs(line, f_out);
        }

        fclose(f_out);
        fclose(f_in);

        chmod(out, EXEC_MASK);
    }
    closedir(d);

    mkdir_with_perms("/system", 0755, NULL, NULL);
    mkdir_with_perms("/data", 0771, "system", "system");
    mkdir_with_perms("/cache", 0770, "system", "cache");

    static const char *folders[2][3] = 
    {
        { "system", "data", "cache" },
        { "system.img", "data.img", "cache.img" },
    };

    unsigned long flags[2][3] = {
        { MS_BIND | MS_RDONLY, MS_BIND, MS_BIND },
        { MS_RDONLY | MS_NOATIME, MS_NOATIME, MS_NOATIME },
    };

    uint32_t i;
    char from[256];
    char to[256];
    int img = (int)(rom->type == ROM_ANDROID_USB_IMG);
    for(i = 0; i < ARRAY_SIZE(folders[0]); ++i)
    {
        sprintf(from, "%s/%s", rom->base_path, folders[img][i]);
        sprintf(to, "/%s", folders[0][i]);

        if(img == 0)
        {
            if(mount(from, to, "ext4", flags[img][i], "") < 0)
            {
                ERROR("Failed to mount %s to %s (%d: %s)", from, to, errno, strerror(errno));
                return -1;
            }
        }
        else
        {
            if(multirom_mount_loop(from, to, flags[img][i]) < 0)
                return -1;
        }
    }
    return 0;
}

int multirom_create_media_link(void)
{
    int media_new = 0;
    int api_level = multirom_get_api_level("/system/build.prop");
    if(api_level <= 0)
        return -1;

    struct stat info;
    if(stat(REALDATA"/media/0", &info) >= 0)
        media_new = 1;

    static const char *paths[] = {
        REALDATA"/media",      // 0
        REALDATA"/media/0",    // 1

        "/data/media",         // 2
        "/data/media/0",       // 3
    };

    int from, to;

    if(api_level <= 16)
    {
        to = 2;
        if(!media_new) from = 0;
        else           from = 1;
    }
    else if(api_level >= 17)
    {
        from = 0;
        if(!media_new) to = 3;
        else           to = 2;
    }

    ERROR("Making media dir: api %d, media_new %d, %s to %s", api_level, media_new, paths[from], paths[to]);
    if(mkdir_recursive(paths[to], 0775) == -1)
    {
        ERROR("Failed to make media dir");
        return -1;
    }

    if(mount(paths[from], paths[to], "ext4", MS_BIND, "") < 0)
    {
        ERROR("Failed to bind media folder %d (%s)", errno, strerror(errno));
        return -1;
    }

    if(api_level >= 17)
    {
        FILE *f = fopen(LAYOUT_VERSION, "w");
        if(!f)
        {
            ERROR("Failed to create .layout_version!\n");
            return -1;
        }
        fputs("2", f);
        fclose(f);
        chmod(LAYOUT_VERSION, 0600);
    }
    return 0;
}

int multirom_get_api_level(const char *path)
{
    FILE *f = fopen(path, "r");
    if(!f)
    {
        ERROR("Could not open %s to read api level!", path);
        return -1;
    }

    int res = -1;
    char line[256];
    while(res == -1 && (fgets(line, sizeof(line), f)))
    {
        if(strstr(line, "ro.build.version.sdk=") == line)
            res = atoi(strchr(line, '=')+1);
    }
    fclose(f);

    if(res == 0)
        ERROR("Invalid ro.build.version.sdk line in build.prop");

    return res;
}

void multirom_take_screenshot(void)
{
    char *buffer = NULL;
    int len = fb_clone(&buffer);

    int counter;
    char path[256];
    struct stat info;
    FILE *f = NULL;

    for(counter = 0; 1; ++counter)
    {
        sprintf(path, "%s/screenshot_%02d.raw", multirom_dir, counter);
        if(stat(path, &info) >= 0)
            continue;

        f = fopen(path, "w");
        if(f)
        {
            fwrite(buffer, 1, len, f);
            fclose(f);
        }
        break;
    }

    free(buffer);

    fb_fill(WHITE);
    fb_update();
    usleep(100000);
    fb_draw();
}

int multirom_get_trampoline_ver(void)
{
    static int ver = -2;
    if(ver == -2)
    {
        ver = -1;

        char *cmd[] = { "/init", "-v", NULL };
        char *res = run_get_stdout(cmd);
        if(res)
        {
            ver = atoi(res);
            free(res);
        }
    }
    return ver;
}

int multirom_has_kexec(void)
{
    static int has_kexec = -2;
    if(has_kexec != -2)
        return has_kexec;

    struct stat info;
    if(stat("/proc/config.gz", &info) < 0)
    {
        has_kexec = -1;
        return has_kexec;
    }

    char *cmd_cp[] = { busybox_path, "cp", "/proc/config.gz", "/config.gz", NULL };
    run_cmd(cmd_cp);

    char *cmd_gzip[] = { busybox_path, "gzip", "-d", "/config.gz", NULL };
    run_cmd(cmd_gzip);

    char *cmd_grep[] = { busybox_path, "grep", "CONFIG_KEXEC_HARDBOOT=y", "/config", NULL };
    if(run_cmd(cmd_grep) == 0)
        has_kexec = 0;
    else
        has_kexec = -1;

    return has_kexec;
}

int multirom_get_cmdline(char *str, size_t size)
{
    FILE *f = fopen("/proc/cmdline", "r");
    if(!f)
        return -1;
    fgets(str, size, f);
    fclose(f);
    return 0;
}

int multirom_find_file(char *res, const char *name_part, const char *path)
{
    DIR *d = opendir(path);
    if(!d)
        return -1;

    int ret= -1;
    struct dirent *dr;
    while(ret == -1 && (dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        if(!strstr(dr->d_name, name_part))
            continue;

        sprintf(res, "%s/%s", path, dr->d_name);
        ret = 0;
    }

    closedir(d);
    return ret;
}

int multirom_load_kexec(struct multirom_rom *rom)
{
    int res = -1;
    // kexec --load-hardboot ./zImage --command-line="$(cat /proc/cmdline)" --mem-min=0xA0000000 --initrd=./rd.img
    //                    0            1                 2            3                       4            5            6
    char *cmd[] = { kexec_path, "--load-hardboot", malloc(1024), "--mem-min=0xA0000000", malloc(1024), malloc(1024), NULL };

    switch(rom->type)
    {
        case ROM_UBUNTU_INTERNAL:
        case ROM_UBUNTU_USB_DIR:
        case ROM_UBUNTU_USB_IMG:
            if(multirom_fill_kexec_ubuntu(rom, cmd) != 0)
                goto exit;
            break;
        case ROM_ANDROID_INTERNAL:
        case ROM_ANDROID_USB_DIR:
        case ROM_ANDROID_USB_IMG:
            if(multirom_fill_kexec_android(rom, cmd) != 0)
                goto exit;
            break;
        default:
            ERROR("Unsupported rom type to kexec (%d)!\n", rom->type);
            goto exit;
    }

    ERROR("Loading kexec: %s %s %s %s %s %s\n", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);
    if(run_cmd(cmd) == 0)
        res = 0;
    else
        ERROR("kexec call failed\n");

    char *cmd_cp[] = { busybox_path, "cp", kexec_path, "/kexec", NULL };
    run_cmd(cmd_cp);
    chmod("/kexec", 0755);

exit:
    free(cmd[2]);
    free(cmd[4]);
    free(cmd[5]);
    return res;
}

int multirom_fill_kexec_ubuntu(struct multirom_rom *rom, char **cmd)
{
    char rom_path[256];
    if(rom->is_in_root == 0)
        sprintf(rom_path, "%s/root/boot", rom->base_path);
    else
        sprintf(rom_path, "%s/boot", REALDATA);

    if(multirom_find_file(cmd[2], "vmlinuz", rom_path) == -1)
    {
        ERROR("Failed to get vmlinuz path\n");
        return -1;
    }

    char str[1000];
    if(multirom_find_file(str, "initrd.img", rom_path) == -1)
    {
        ERROR("Failed to get initrd path\n");
        return -1;
    }

    sprintf(cmd[4], "--initrd=%s", str);

    if(multirom_get_cmdline(str, sizeof(str)) == -1)
    {
        ERROR("Failed to get cmdline\n");
        return -1;
    }

    // TODO correct root
    sprintf(cmd[5], "--command-line=%s root=/dev/mmcblk0p9 ro console=tty1 fbcon=rotate:1 quiet", str);
    return 0;
}

int multirom_fill_kexec_android(struct multirom_rom *rom, char **cmd)
{
    int res = -1;
    char img_path[256];
    sprintf(img_path, "%s/boot.img", rom->base_path);

    FILE *f = fopen(img_path, "r");
    if(!f)
    {
        ERROR("kexec_fill could not open boot image (%s)!", img_path);
        return -1;
    }

    struct boot_img_hdr header;
    fread(header.magic, 1, sizeof(struct boot_img_hdr), f);

    unsigned p = header.page_size;

    fseek(f, p, SEEK_SET); // seek to kernel
    if(multirom_extract_bytes("/zImage", f, header.kernel_size) != 0)
        goto exit;

    fseek(f, p + ((header.kernel_size + p - 1) / p)*p, SEEK_SET); // seek to ramdisk
    if(multirom_extract_bytes("/initrd.img", f, header.ramdisk_size) != 0)
        goto exit;

    char cmdline[1024];
    if(multirom_get_cmdline(cmdline, sizeof(cmdline)) == -1)
    {
        ERROR("Failed to get cmdline\n");
        return -1;
    }

    strcpy(cmd[2], "/zImage");
    strcpy(cmd[4], "--initrd=/initrd.img");
    sprintf(cmd[5], "--command-line=%s %s", cmdline, header.cmdline);

    res = 0;
exit:
    fclose(f);
    return res;
}

int multirom_extract_bytes(const char *dst, FILE *src, size_t size)
{
    FILE *f = fopen(dst, "w");
    if(!f)
    {
        ERROR("Failed to open dest file %s\n", dst);
        return -1;
    }

    char *buff = malloc(size);

    fread(buff, 1, size, src);
    fwrite(buff, 1, size, f);

    fclose(f);
    free(buff);
    return 0;
}

void multirom_destroy_partition(void *part)
{
    struct usb_partition *p = (struct usb_partition *)part;
    if(p->mount_path && p->keep_mounted == 0)
        umount(p->mount_path);

    free(p->name);
    free(p->uuid);
    free(p->mount_path);
    free(p->fs);
    free(p);
}

int multirom_update_partitions(struct multirom_status *s)
{
    pthread_mutex_lock(&parts_mutex);

    list_clear(&s->partitions, &multirom_destroy_partition);

    char *cmd[] = { busybox_path, "blkid", NULL };
    char *res = run_get_stdout(cmd);
    if(!res)
    {
        pthread_mutex_unlock(&parts_mutex);
        return -1;
    }

    char *p = strtok(res, "\n");
    while(p != NULL)
    {
        if(!strstr(p, "/sd"))
        {
            p = strtok(NULL, "\n");
            continue;
        }

        struct usb_partition *part = malloc(sizeof(struct usb_partition));
        memset(part, 0, sizeof(struct usb_partition));

        char *t = strndup(p, strchr(p, ':') - p);
        part->name = strdup(strrchr(t, '/')+1);

        t = strstr(p, "UUID=\"");
        if(t)
        {
            t += strlen("UUID=\"");
            part->uuid = strndup(t, strchr(t, '"') - t);
        }

        t = strstr(p, "TYPE=\"");
        if(t)
        {
            t += strlen("TYPE=\"");

            part->fs = strndup(t, strchr(t, '"') - t);
        }

        if(multirom_mount_usb(part) == 0)
        {
            list_add(part, &s->partitions);
            ERROR("Found part %s: %s, %s\n", part->name, part->uuid, part->fs);
        }
        else
        {
            ERROR("Failed to mount part %s %s, %s\n", part->name, part->uuid, part->fs);
            multirom_destroy_partition(part);
        }

        p = strtok(NULL, "\n");
    }
    pthread_mutex_unlock(&parts_mutex);
    free(res);
    return 0;
}

int multirom_mount_usb(struct usb_partition *part)
{
    mkdir("/mnt", 0777);

    char path[256];
    sprintf(path, "/mnt/%s", part->name);
    if(mkdir(path, 0777) != 0 && errno != EEXIST)
    {
        ERROR("Failed to create dir for mount %s\n", path);
        return -1;
    }

    char src[256];
    sprintf(src, "/dev/block/%s", part->name);

    if(mount(src, path, part->fs, MS_NOATIME, "") < 0)
    {
        ERROR("Failed to mount %s (%d: %s)\n", src, errno, strerror(errno));
        return -1;
    }
    part->mount_path = strdup(path);
    return 0;
}

void *multirom_usb_refresh_thread_work(void *status)
{
    uint32_t timer = 0;
    time_t last_change = 0;
    struct stat info;

    while(run_usb_refresh)
    {
        if(timer <= 50)
        {
            if(stat("/dev/block", &info) >= 0 && info.st_ctime > last_change)
            {
                multirom_update_partitions((struct multirom_status*)status);
                if(usb_refresh_handler)
                    (*usb_refresh_handler)();
                last_change = info.st_ctime;
            }
            timer = 500;
        }
        else
            timer -= 50;
        usleep(50000);
    }
    return NULL;
}

void multirom_set_usb_refresh_thread(struct multirom_status *s, int run)
{
    if(run_usb_refresh == run)
        return;

    run_usb_refresh = run;
    if(run)
        pthread_create(&usb_refresh_thread, NULL, multirom_usb_refresh_thread_work, s);
    else
        pthread_join(usb_refresh_thread, NULL);
}

void multirom_set_usb_refresh_handler(void (*handler)(void))
{
    usb_refresh_handler = handler;
}

int multirom_mount_loop(const char *src, const char *dst, int flags)
{
    int file_fd, device_fd, res = -1;

    file_fd = open(src, O_RDWR);
    if (file_fd < 0) {
        ERROR("Failed to open image %s\n", src);
        return -1;
    }

    static int loop_devs = 0;
    char path[64];
    sprintf(path, "/dev/loop%d", loop_devs);
    if(mknod(path, S_IFBLK | 0777, makedev(7, loop_devs)) < 0)
    {
        ERROR("Failed to create loop file (%d: %s)\n", errno, strerror(errno));
        goto close_file;
    }

    ++loop_devs;

    device_fd = open(path, O_RDWR);
    if (device_fd < 0)
    {
        ERROR("Failed to open loop file (%d: %s)\n", errno, strerror(errno));
        goto close_file;
    }

    if (ioctl(device_fd, LOOP_SET_FD, file_fd) < 0)
    {
        ERROR("ioctl LOOP_SET_FD failed on %s\n", path);
        goto close_dev;
    }

    if(mount(path, dst, "ext4", flags, "") < 0)
        ERROR("Failed to mount loop (%d: %s)\n", errno, strerror(errno));
    else
        res = 0;

close_dev:
    close(device_fd);
close_file:
    close(file_fd);
    return res;
}