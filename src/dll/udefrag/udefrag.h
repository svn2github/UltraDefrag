/*
 *  UltraDefrag - a powerful defragmentation tool for Windows NT.
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

/*
* udefrag.dll interface header.
*/

#ifndef _UDEFRAG_H_
#define _UDEFRAG_H_

#if defined(__cplusplus)
extern "C" {
#endif

/* debug print levels */
#define DBG_NORMAL     0
#define DBG_DETAILED   1
#define DBG_PARANOID   2

/* UltraDefrag error codes */
#define UDEFRAG_UNKNOWN_ERROR     (-1)
#define UDEFRAG_NO_MEM            (-4)
#define UDEFRAG_CDROM             (-5)
#define UDEFRAG_REMOTE            (-6)
#define UDEFRAG_ASSIGNED_BY_SUBST (-7)
#define UDEFRAG_REMOVABLE         (-8)
#define UDEFRAG_UDF_DEFRAG        (-9)
#define UDEFRAG_DIRTY_VOLUME      (-12)

#define DEFAULT_REFRESH_INTERVAL 100

#define MAX_DOS_DRIVES 26
#define MAXFSNAME      32  /* I think, that's enough */

int udefrag_init_library(void);
void udefrag_unload_library(void);

typedef struct _volume_info {
    char letter;
    char fsname[MAXFSNAME];
    wchar_t label[MAX_PATH + 1];
    LARGE_INTEGER total_space;
    LARGE_INTEGER free_space;
    int is_removable;
    int is_dirty;
} volume_info;

volume_info *udefrag_get_vollist(int skip_removable);
void udefrag_release_vollist(volume_info *v);
int udefrag_validate_volume(char volume_letter,int skip_removable);
int udefrag_get_volume_information(char volume_letter,volume_info *v);

typedef enum {
    ANALYSIS_JOB = 0,
    DEFRAGMENTATION_JOB,
    FULL_OPTIMIZATION_JOB,
    QUICK_OPTIMIZATION_JOB,
    MFT_OPTIMIZATION_JOB
} udefrag_job_type;

typedef enum {
    VOLUME_ANALYSIS = 0,     /* should be zero */
    VOLUME_DEFRAGMENTATION,
    VOLUME_OPTIMIZATION
} udefrag_operation_type;

/* flags triggering algorithm features */
#define UD_JOB_REPEAT                     0x1
/*
* 0x2, 0x4, 0x8 flags have been used 
* in the past for experimental options
*/
#define UD_JOB_CONTEXT_MENU_HANDLER       0x10

enum {
    UNUSED_MAP_SPACE = 0,        /* has the lowest precedence */
    FREE_SPACE,                  
    SYSTEM_SPACE,
    SYSTEM_OVER_LIMIT_SPACE,
    FRAGM_SPACE,
    FRAGM_OVER_LIMIT_SPACE,
    UNFRAGM_SPACE,
    UNFRAGM_OVER_LIMIT_SPACE,
    DIR_SPACE,
    DIR_OVER_LIMIT_SPACE,
    COMPRESSED_SPACE,
    COMPRESSED_OVER_LIMIT_SPACE,
    MFT_ZONE_SPACE,
    MFT_SPACE,                   /* has the highest precedence */
    SPACE_STATES                 /* this member must always be the last one */
};

#define UNKNOWN_SPACE FRAGM_SPACE

typedef struct _udefrag_progress_info {
    unsigned long files;              /* number of files */
    unsigned long directories;        /* number of directories */
    unsigned long compressed;         /* number of compressed files */
    unsigned long fragmented;         /* number of fragmented files */
    ULONGLONG fragments;              /* number of fragments */
    ULONGLONG bad_fragments;          /* number of fragments which need to be joined together */
    double fragmentation;             /* fragmentation percentage */
    ULONGLONG total_space;            /* volume size, in bytes */
    ULONGLONG free_space;             /* free space amount, in bytes */
    ULONGLONG mft_size;               /* mft size, in bytes */
    udefrag_operation_type current_operation;  /* identifies the currently running operation */
    unsigned long pass_number;        /* the current disk processing pass, increases 
                                         immediately after the pass completion */
    ULONGLONG clusters_to_process;    /* number of clusters to process */
    ULONGLONG processed_clusters;     /* number of already processed clusters */
    double percentage;                /* job completion percentage */
    int completion_status;            /* zero for running jobs, positive value for succeeded, negative for failed */
    char *cluster_map;                /* the cluster map */
    int cluster_map_size;             /* size of the cluster map, in bytes */
    ULONGLONG moved_clusters;         /* number of moved clusters */
    ULONGLONG total_moves;            /* number of moves made by move_files_to_front/back functions */
} udefrag_progress_info;

typedef void  (*udefrag_progress_callback)(udefrag_progress_info *pi, void *p);
typedef int   (*udefrag_terminator)(void *p);

int udefrag_start_job(char volume_letter,udefrag_job_type job_type,int flags,
    int cluster_map_size,udefrag_progress_callback cb,udefrag_terminator t,void *p);

char *udefrag_get_results(udefrag_progress_info *pi);
void udefrag_release_results(char *results);

char *udefrag_get_error_description(int error_code);

int udefrag_set_log_file_path(void);

#if defined(__cplusplus)
}
#endif

#endif /* _UDEFRAG_H_ */
