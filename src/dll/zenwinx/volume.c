/*
 *  ZenWINX - WIndows Native eXtended library.
 *  Copyright (c) 2007-2016 Dmitri Arkhangelski (dmitriar@gmail.com).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/**
 * @file volume.c
 * @brief Disk volumes.
 * @addtogroup Disks
 * @{
 */

#include "ntndk.h"
#include "zenwinx.h"

/**
 * @internal
 * @brief Opens the root directory of a volume.
 */
static HANDLE OpenRootDirectory(unsigned char volume_letter)
{
    wchar_t rootpath[] = L"\\??\\A:\\";
    HANDLE hRoot;
    NTSTATUS status;
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;

    rootpath[4] = (wchar_t)winx_toupper(volume_letter);
    RtlInitUnicodeString(&us,rootpath);
    InitializeObjectAttributes(&ObjectAttributes,&us,
                   FILE_READ_ATTRIBUTES,NULL,NULL);
    status = NtCreateFile(&hRoot,FILE_GENERIC_READ,
                &ObjectAttributes,&IoStatusBlock,NULL,0,
                FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,0,
                NULL,0);
    if(!NT_SUCCESS(status)){
        strace(status,"cannot open %ls",rootpath);
        return NULL;
    }
    return hRoot;
}

/**
 * @brief A native equivalent of Win32 GetDriveType().
 * @param[in] letter the volume letter
 * @return Type of the drive, negative values indicate failure.
 */
int winx_get_drive_type(char letter)
{
    wchar_t link_name[] = L"\\??\\A:";
    #define MAX_TARGET_LENGTH 256
    wchar_t link_target[MAX_TARGET_LENGTH];
    PROCESS_DEVICEMAP_INFORMATION pdi;
    FILE_FS_DEVICE_INFORMATION ffdi;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    int drive_type;
    HANDLE hRoot;

    /* The additional checks for DFS were suggested by Stefan Pendl (pendl2megabit@yahoo.de). */
    /* DFS shares have DRIVE_NO_ROOT_DIR type though they are actually remote. */

    letter = winx_toupper(letter);
    if(letter < 'A' || letter > 'Z'){
        etrace("invalid letter %c",letter);
        return (-1);
    }
    
    /* check for the drive existence */
    link_name[4] = (wchar_t)letter;
    if(winx_query_symbolic_link(link_name,link_target,MAX_TARGET_LENGTH) < 0)
        return (-1);
    
    /* check for an assignment made by the subst command */
    if(wcsstr(link_target,L"\\??\\") == (wchar_t *)link_target)
        return DRIVE_ASSIGNED_BY_SUBST_COMMAND;

    /* check for classical floppies */
    if(wcsstr(link_target,L"Floppy"))
        return DRIVE_REMOVABLE;
    
    /* try to define exactly which type has the specified drive */
    RtlZeroMemory(&pdi,sizeof(PROCESS_DEVICEMAP_INFORMATION));
    status = NtQueryInformationProcess(NtCurrentProcess(),
                    ProcessDeviceMap,&pdi,
                    sizeof(PROCESS_DEVICEMAP_INFORMATION),
                    NULL);
    if(NT_SUCCESS(status)){
        drive_type = (int)pdi.Query.DriveType[letter - 'A'];
        /*
        * Type DRIVE_NO_ROOT_DIR have the following drives:
        * 1. assigned by subst command
        * 2. SCSI external drives
        * 3. RAID volumes
        * 4. DFS shares
        * We need additional checks to know exactly.
        */
        if(drive_type != DRIVE_NO_ROOT_DIR)
            return drive_type;
    } else {
        strace(status,"cannot get device map");
        return (-1);
    }
    
    /* try to define exactly again which type has the specified drive */
    /* note that the drive motor can be powered on during this check */
    hRoot = OpenRootDirectory(letter);
    if(hRoot == NULL)
        return (-1);
    RtlZeroMemory(&ffdi,sizeof(FILE_FS_DEVICE_INFORMATION));
    status = NtQueryVolumeInformationFile(hRoot,&iosb,
                    &ffdi,sizeof(FILE_FS_DEVICE_INFORMATION),
                    FileFsDeviceInformation);
    NtClose(hRoot);
    if(!NT_SUCCESS(status)){
        strace(status,"cannot get volume type for \'%c\'",letter);
        return (-1);
    }

    /* detect remote/cd/dvd/unknown drives */
    if(ffdi.Characteristics & FILE_REMOTE_DEVICE)
        return DRIVE_REMOTE;
    switch(ffdi.DeviceType){
    case FILE_DEVICE_CD_ROM:
    case FILE_DEVICE_CD_ROM_FILE_SYSTEM:
    case FILE_DEVICE_DVD:
        return DRIVE_CDROM;
    case FILE_DEVICE_NETWORK_FILE_SYSTEM:
    case FILE_DEVICE_NETWORK: /* ? */
    case FILE_DEVICE_NETWORK_BROWSER: /* ? */
    case FILE_DEVICE_DFS_FILE_SYSTEM:
    case FILE_DEVICE_DFS_VOLUME:
    case FILE_DEVICE_DFS:
        return DRIVE_REMOTE;
    case FILE_DEVICE_UNKNOWN:
        return DRIVE_UNKNOWN;
    }

    /* detect removable disks */
    if(ffdi.Characteristics & FILE_REMOVABLE_MEDIA)
        return DRIVE_REMOVABLE;

    /* detect fixed disks */
    switch(ffdi.DeviceType){
    case FILE_DEVICE_DISK:
    case FILE_DEVICE_FILE_SYSTEM: /* ? */
    /*case FILE_DEVICE_VIRTUAL_DISK:*/
    /*case FILE_DEVICE_MASS_STORAGE:*/
    case FILE_DEVICE_DISK_FILE_SYSTEM:
        return DRIVE_FIXED;
    default:
        break;
    }
    
    /* nothing detected => drive type is unknown */
    return DRIVE_UNKNOWN;
}

/**
 * @internal
 * @brief Retrieves geometry of a drive.
 * @param[in] hRoot handle of the root directory.
 * @param[out] v pointer to structure receiving the information.
 * @return Zero for success, negative value otherwise.
 */
static int get_drive_geometry(HANDLE hRoot,winx_volume_information *v)
{
    FILE_FS_SIZE_INFORMATION ffs;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS status;
    WINX_FILE *f;
    DISK_GEOMETRY dg;
    char buffer[32];
    
    /* get drive geometry */
    RtlZeroMemory(&ffs,sizeof(FILE_FS_SIZE_INFORMATION));
    status = NtQueryVolumeInformationFile(hRoot,&IoStatusBlock,&ffs,
                sizeof(FILE_FS_SIZE_INFORMATION),FileFsSizeInformation);
    if(!NT_SUCCESS(status)){
        strace(status,"cannot get geometry of drive %c:",v->volume_letter);
        return (-1);
    }
    
    /* fill all geometry related fields of the output structure */
    v->total_bytes = (ULONGLONG)ffs.TotalAllocationUnits.QuadPart * \
        ffs.SectorsPerAllocationUnit * ffs.BytesPerSector;
    v->free_bytes = (ULONGLONG)ffs.AvailableAllocationUnits.QuadPart * \
        ffs.SectorsPerAllocationUnit * ffs.BytesPerSector;
    v->total_clusters = (ULONGLONG)ffs.TotalAllocationUnits.QuadPart;
    v->bytes_per_cluster = ffs.SectorsPerAllocationUnit * ffs.BytesPerSector;
    v->sectors_per_cluster = ffs.SectorsPerAllocationUnit;
    v->bytes_per_sector = ffs.BytesPerSector;
    
    /* optional: get device capacity */
    v->device_capacity = 0;
    f = winx_vopen(v->volume_letter);
    if(f != NULL){
        if(winx_ioctl(f,IOCTL_DISK_GET_DRIVE_GEOMETRY,
          "get_drive_geometry: device geometry request",NULL,0,
          &dg,sizeof(dg),NULL) >= 0){
            v->device_capacity = dg.Cylinders.QuadPart * \
                dg.TracksPerCylinder * dg.SectorsPerTrack * dg.BytesPerSector;
            winx_bytes_to_hr(v->device_capacity,1,buffer,sizeof(buffer));
            itrace("%c: device capacity = %s",v->volume_letter,buffer);
        }
        winx_fclose(f);
    }
    return 0;
}

/**
 * @internal
 * @brief Retrieves file system name for the specified volume.
 * @param[in] hRoot handle of the root directory.
 * @param[out] v pointer to structure receiving the name.
 * @return Zero for success, negative value otherwise.
 * @note We could analyze the first sector of the 
 * partition directly, but this method is not so swift
 * as it accesses the disk physically.
 */
static int get_filesystem_name(HANDLE hRoot,winx_volume_information *v)
{
    FILE_FS_ATTRIBUTE_INFORMATION *pfa;
    int fs_attr_info_size;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS status;
    wchar_t fs_name[MAX_FS_NAME_LENGTH + 1];
    int length;

    fs_attr_info_size = MAX_PATH * sizeof(WCHAR) + sizeof(FILE_FS_ATTRIBUTE_INFORMATION);
    pfa = winx_malloc(fs_attr_info_size);
    
    RtlZeroMemory(pfa,fs_attr_info_size);
    status = NtQueryVolumeInformationFile(hRoot,&IoStatusBlock,pfa,
                fs_attr_info_size,FileFsAttributeInformation);
    if(!NT_SUCCESS(status)){
        strace(status,"cannot get file system name of drive %c:",v->volume_letter);
        winx_free(pfa);
        return (-1);
    }
    
    /*
    * pfa->FileSystemName.Buffer may be not NULL terminated
    * (theoretically), so the name extraction is more tricky
    * than it should be.
    */
    length = min(MAX_FS_NAME_LENGTH,pfa->FileSystemNameLength / sizeof(wchar_t));
    wcsncpy(fs_name,pfa->FileSystemName,length);
    fs_name[length] = 0;
    _snprintf(v->fs_name,MAX_FS_NAME_LENGTH,"%ws",fs_name);
    v->fs_name[MAX_FS_NAME_LENGTH] = 0;

    /* cleanup */
    winx_free(pfa);
    return 0;
}

/**
 * @internal
 * @brief Retrieves NTFS specific data for the specified volume.
 * @param[in,out] v pointer to structure receiving the information.
 * @return Zero for success, negative value otherwise.
 */
static int get_ntfs_data(winx_volume_information *v)
{
    WINX_FILE *f;
    int result;
    
    /* open the volume */
    f = winx_vopen(v->volume_letter);
    if(f == NULL)
        return (-1);
    
    /* get ntfs data */
    result = winx_ioctl(f,FSCTL_GET_NTFS_VOLUME_DATA,
      "get_ntfs_data: ntfs data request",NULL,0,
      &v->ntfs_data,sizeof(NTFS_DATA),NULL);
    winx_fclose(f);
    return result;
}

/**
 * @internal
 * @brief Retrieves the label of the specified volume.
 * @param[in] hRoot handle of the root directory.
 * @param[out] v pointer to structure receiving the label.
 */
static void get_volume_label(HANDLE hRoot,winx_volume_information *v)
{
    FILE_FS_VOLUME_INFORMATION *ffvi;
    int buffer_size;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS status;
    
    /* reset label */
    v->label[0] = 0;
    
    /* allocate memory */
    buffer_size = (sizeof(FILE_FS_VOLUME_INFORMATION) - sizeof(wchar_t)) + (MAX_PATH + 1) * sizeof(wchar_t);
    ffvi = winx_malloc(buffer_size);
    
    /* try to get actual label */
    RtlZeroMemory(ffvi,buffer_size);
    status = NtQueryVolumeInformationFile(hRoot,&IoStatusBlock,ffvi,
                buffer_size,FileFsVolumeInformation);
    if(!NT_SUCCESS(status)){
        strace(status,"cannot get volume label of drive %c:",
            v->volume_letter);
        winx_free(ffvi);
        return;
    }
    wcsncpy(v->label,ffvi->VolumeLabel,MAX_PATH);
    v->label[MAX_PATH] = 0;
    winx_free(ffvi);
}

/**
 * @internal
 * @brief Retrieves the dirty flag for the specified volume.
 * @param[in,out] v pointer to structure receiving the flag.
 */
static void get_volume_dirty_flag(winx_volume_information *v)
{
    WINX_FILE *f;
    ULONG dirty_flag;
    int result;
    
    /* open the volume */
    f = winx_vopen(v->volume_letter);
    if(f == NULL) return;
    
    /* get dirty flag */
    result = winx_ioctl(f,FSCTL_IS_VOLUME_DIRTY,
        "get_volume_dirty_flag: dirty flag request",
        NULL,0,&dirty_flag,sizeof(ULONG),NULL);
    winx_fclose(f);
    if(result >= 0 && (dirty_flag & VOLUME_IS_DIRTY)){
        etrace("%c: volume is dirty! Run CHKDSK to repair it.",
            v->volume_letter);
        v->is_dirty = 1;
    }
}

/**
 * @brief Retrieves detailed information about the specified volume.
 * @param[in] volume_letter the volume letter.
 * @param[in,out] v pointer to structure receiving the information.
 * @return Zero for success, negative value otherwise.
 */
int winx_get_volume_information(char volume_letter,winx_volume_information *v)
{
    HANDLE hRoot;
    
    /* check input data correctness */
    if(v == NULL)
        return (-1);

    /* ensure that it will work on w2k */
    volume_letter = winx_toupper(volume_letter);
    
    /* reset all fields of the structure, except of volume_letter */
    memset(v,0,sizeof(winx_volume_information));
    v->volume_letter = volume_letter;

    if(volume_letter < 'A' || volume_letter > 'Z')
        return (-1);
    
    /* open the root directory */
    hRoot = OpenRootDirectory(volume_letter);
    if(hRoot == NULL)
        return (-1);
    
    /* get drive geometry */
    if(get_drive_geometry(hRoot,v) < 0){
        NtClose(hRoot);
        return (-1);
    }
    
    /* get name of the file system */
    if(get_filesystem_name(hRoot,v) < 0){
        NtClose(hRoot);
        return (-1);
    }
    
    /* get name of the volume */
    get_volume_label(hRoot,v);

    /* get NTFS data */
    memset(&v->ntfs_data,0,sizeof(NTFS_DATA));
    if(!strcmp(v->fs_name,"NTFS")){
        if(get_ntfs_data(v) < 0){
            etrace("NTFS data is unavailable for %c:",
                volume_letter);
        }
    }
    
    /* get dirty flag */
    get_volume_dirty_flag(v);
    
    NtClose(hRoot);
    return 0;
}

/**
 * @brief Opens a volume for read access.
 * @param[in] volume_letter the volume letter.
 * @return The descriptor of the opened volume,
 * NULL indicates failure.
 */
WINX_FILE *winx_vopen(char volume_letter)
{
    wchar_t path[] = L"\\??\\A:";

    path[4] = winx_toupper(volume_letter);
    return winx_fopen(path,"r");
}

/**
 * @brief fflush equivalent for entire volume.
 */
int winx_vflush(char volume_letter)
{
    wchar_t path[] = L"\\??\\A:";
    WINX_FILE *f;
    int result = -1;
    
    path[4] = winx_toupper(volume_letter);
    f = winx_fopen(path,"r+");
    if(f){
        result = winx_fflush(f);
        winx_fclose(f);
    }

    return result;
}

/**
 * @internal
 * @brief Defines sorting rules for trees of free space regions.
 */
static int compare_regions(const void *prb_a, const void *prb_b, void *prb_param)
{
    winx_volume_region *a, *b;
    
    a = (winx_volume_region *)prb_a;
    b = (winx_volume_region *)prb_b;
    
    /* sort regions by LCN in ascending order */
    if(a->lcn < b->lcn) return (-1);
    if(a->lcn == b->lcn) return 0;
    return 1;
}

/**
 * @internal
 * @brief Releases memory allocated for a single tree item.
 */
static void free_item(void *prb_item, void *prb_param)
{
    winx_volume_region *rgn = (winx_volume_region *)prb_item;
    winx_free(rgn);
}

/**
 * @brief Enumerates free regions on the specified volume.
 * @param[in] volume_letter the volume letter.
 * @param[in] start_lcn the logical cluster number to start with.
 * @param[in] length number of clusters to scan.
 * @param[in] flags a combination of WINX_GVR_xxx flags.
 * @param[in] cb the address of the procedure to be called
 * each time when a free region is found on the volume.
 * If the callback procedure returns nonzero value,
 * the scan terminates immediately.
 * @param[in] user_defined_data pointer to data
 * to be passed to the registered callback.
 * @return Binary tree of the free space
 * regions, NULL indicates failure.
 * @note
 * - Whenever a file gets moved by the FSCTL_MOVE_FILE request on a volume
 * formatted in NTFS Windows refuses to release clusters which belonged to
 * the file immediately. However, a call to winx_get_free_volume_regions
 * forces Windows to release all those clusters within the scanned region.
 * - This function as well as others guarantee that all the
 * regions follow each other and none of them has zero length.
 * @par Example:
 * @code
 * char volume_letter = 'c';
 * winx_volume_information v;
 * struct prb_table *free_regions;
 * struct prb_traverser t;
 * winx_volume_region *rgn;
 *
 * // get total number of clusters to scan
 * if(winx_get_volume_information(volume_letter,&v) == 0){
 *     // enumerate all free regions on the volume
 *     free_regions = winx_get_free_volume_regions(
 *         volume_letter,0,v.total_clusters,0,NULL,NULL);
 *     if(free_regions){
 *         // loop through the list of regions
 *         prb_t_init(&t,free_regions);
 *         while(1){
 *             rgn = prb_t_next(&t);
 *             if(rgn == NULL) break;
 *
 *             dtrace("LCN: %I64u, LENGTH: %I64u",
 *                 rgn->lcn,rgn->length);
 *         }
 *     }
 * }
 * @endcode
 */
struct prb_table *winx_get_free_volume_regions(char volume_letter,
        ULONGLONG start_lcn, ULONGLONG length, int flags,
        volume_region_callback cb, void *user_defined_data)
{
    struct prb_table *regions = NULL;
    winx_volume_region *rgn = NULL;
    BITMAP_DESCRIPTOR *bitmap;
    #define LLINVALID   ((ULONGLONG) -1)
    #define BITMAPBYTES 4096
    #define BITMAPSIZE  (BITMAPBYTES + 2 * sizeof(ULONGLONG))
    /* bit shifting array for efficient processing of the bitmap */
    unsigned char bitshift[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    WINX_FILE *f;
    ULONGLONG i, n, start, next, free_rgn_start;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    /* ensure that it will work on w2k */
    volume_letter = winx_toupper(volume_letter);
    
    /* allocate memory */
    bitmap = winx_malloc(BITMAPSIZE);
    regions = prb_create(compare_regions,NULL,NULL);
    
    /* open the volume */
    f = winx_vopen(volume_letter);
    if(f == NULL){
        prb_destroy(regions,free_item);
        winx_free(bitmap);
        return NULL;
    }
    
    /* get volume bitmap */
    next = start_lcn, free_rgn_start = LLINVALID;
    do {
        /* get next portion of the bitmap */
        memset(bitmap,0,BITMAPSIZE);
        status = NtFsControlFile(winx_fileno(f),NULL,NULL,0,&iosb,
            FSCTL_GET_VOLUME_BITMAP,&next,sizeof(ULONGLONG),bitmap,
            BITMAPSIZE);
        if(NT_SUCCESS(status)){
            NtWaitForSingleObject(winx_fileno(f),FALSE,NULL);
            status = iosb.Status;
        }
        if(status != STATUS_SUCCESS && status != STATUS_BUFFER_OVERFLOW){
            strace(status,"cannot get volume bitmap");
            winx_fclose(f);
            winx_free(bitmap);
            if(flags & WINX_GVR_ALLOW_PARTIAL_SCAN){
                return regions;
            } else {
                prb_destroy(regions,free_item);
                return NULL;
            }
        }
        
        /* scan through the returned bitmap info */
        start = bitmap->StartLcn;
        n = min(BITMAPBYTES * 8, bitmap->ClustersToEndOfVol);
        if(next - start + length < n) n = next - start + length;
        for(i = next - start; i < n; i++){
            if(!(bitmap->Map[ i/8 ] & bitshift[ i % 8 ])){
                /* cluster is free */
                if(free_rgn_start == LLINVALID)
                    free_rgn_start = start + i;
            } else {
                /* cluster isn't free */
                if(free_rgn_start != LLINVALID){
                    /* add free region to the tree */
                    rgn = winx_malloc(sizeof(winx_volume_region));
                    rgn->lcn = free_rgn_start;
                    rgn->length = start + i - free_rgn_start;
                    (void)prb_insert(regions,(void *)rgn);
                    if(cb != NULL){
                        if(cb(rgn,user_defined_data))
                            goto done;
                    }
                    free_rgn_start = LLINVALID;
                }
            }
        }
        
        /* break if we have scanned all the requested clusters */
        if(length <= i - (next - start)) break;
        length -= i - (next - start);

        /* go to the next portion of data */
        next = bitmap->StartLcn + i;
    } while(status != STATUS_SUCCESS);

    if(free_rgn_start != LLINVALID){
        /* add free region to the tree */
        rgn = winx_malloc(sizeof(winx_volume_region));
        rgn->lcn = free_rgn_start;
        rgn->length = start + i - free_rgn_start;
        (void)prb_insert(regions,(void *)rgn);
        if(cb != NULL){
            if(cb(rgn,user_defined_data))
                goto done;
        }
        free_rgn_start = LLINVALID;
    }

done:    
    /* cleanup */
    winx_fclose(f);
    winx_free(bitmap);
    return regions;
}

/**
 * @brief Adds a region to the
 * specified tree of regions.
 * @param[in,out] regions the tree of regions.
 * @param[in] lcn the logical cluster number
 * of the region to be added.
 * @param[in] length size of the region
 * to be added, in clusters.
 * @return The region the added
 * range of clusters belongs to.
 */
winx_volume_region *winx_add_volume_region(struct prb_table *regions,
        ULONGLONG lcn,ULONGLONG length)
{
    winx_volume_region *rgn, *item, *prev, *next;
    struct prb_traverser t;
    
    /* validate parameters */
    if(regions == NULL || length == 0) return NULL;
    
    /* allocate memory */
    rgn = winx_malloc(sizeof(winx_volume_region));
    
    /* try to insert the region */
    rgn->lcn = lcn, rgn->length = length;
    item = prb_t_insert(&t,regions,(void *)rgn);
    
    if(item != rgn){
        /* a duplicate found */
        winx_free(rgn); rgn = item;
        if(rgn->length < length)
            rgn->length = length;
    } else {
        /*
        * The region inserted successfully,
        * let's check whether it hits the
        * previous one or not.
        */
        prev = prb_t_prev(&t);
        if(prev){
            if(lcn <= prev->lcn + prev->length){
                if(lcn + length > prev->lcn + prev->length)
                    prev->length = lcn + length - prev->lcn;
                prb_delete(regions,rgn);
                winx_free(rgn);
                rgn = prev;
            }
        }
        
        /* advance traverser back */
        if(rgn != prev) prb_t_next(&t);
    }

    /*
    * Check whether the region hits
    * the following regions or not.
    */
    while(1){
        next = prb_t_next(&t);
        if(next == NULL) break;

        /* the region follows the inserted one */
        if(next->lcn > rgn->lcn + rgn->length) break;
        
        /* the region hits/overlaps the inserted one */
        if(next->lcn + next->length > rgn->lcn + rgn->length){
            rgn->length = next->lcn + next->length - rgn->lcn;
            prb_delete(regions,next);
            winx_free(next);
            break;
        }
        
        /* the region is inside the inserted one */
        prb_t_prev(&t);
        prb_delete(regions,next);
        winx_free(next);
    }

    return rgn;
}

/**
 * @brief Subtracts a region from
 * the specified tree of regions.
 * @param[in,out] regions the tree of regions.
 * @param[in] lcn the logical cluster number
 * of the region to be subtracted.
 * @param[in] length size of the region
 * to be subtracted, in clusters.
 */
void winx_sub_volume_region(struct prb_table *regions,
        ULONGLONG lcn,ULONGLONG length)
{
    winx_volume_region *rgn, *add_rgn;

    /* validate parameters */
    if(regions == NULL || length == 0) return;
    
    rgn = winx_add_volume_region(regions,lcn,length);
    
    /*
    * Now all the clusters to be subtracted should
    * belong to a single region pointed by rgn.
    * Let's check it, however, for extra safety.
    */
    if(lcn < rgn->lcn || lcn + length > rgn->lcn + rgn->length){
        etrace("winx_add_volume_region failed to do its job");
        return; /* this point will be reached only in case of bugs presence */
    }
    
    /* let's cut off the region */
    if(lcn > rgn->lcn){
        if(lcn + length == rgn->lcn + rgn->length){
            /* cut off the end of the region */
            rgn->length -= length;
        } else {
            /* cut off middle part of the region */
            add_rgn = winx_malloc(sizeof(winx_volume_region));
            add_rgn->lcn = lcn + length;
            add_rgn->length = rgn->lcn + rgn->length - add_rgn->lcn;
            (void)prb_insert(regions,(void *)add_rgn);
            rgn->length = lcn - rgn->lcn;
        }
    } else {
        if(lcn + length == rgn->lcn + rgn->length){
            /* remove the entire region */
            prb_delete(regions,rgn);
            winx_free(rgn);
        } else {
            /* cut off the beginning of the region */
            rgn->lcn += length; rgn->length -= length;
        }
    }
}

/**
 * @brief Releases memory allocated
 * by winx_get_free_volume_regions.
 */
void winx_release_free_volume_regions(struct prb_table *regions)
{
    if(regions) prb_destroy(regions,free_item);
}

/** @} */
