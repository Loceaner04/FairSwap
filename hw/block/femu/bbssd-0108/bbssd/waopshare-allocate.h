#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H
#include <time.h>
#include "../nvme.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))
#define period 3600
#define COLD 0
#define HOT 1
#define WARM 2
#define NUM_DATA 3
#define REST_TIME 5
#define MEASURE_TIME 5
#define ALLOCATE_TIME 5
#define NONE -1
#define REST 0
#define MEASURE 1
#define ALLOCATE 2

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    // double gc_thres_pcent;
    // int gc_thres_lines;
    // double gc_thres_pcent_high;
    // int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    int user_id;
    uint64_t T_written;
    uint64_t alt;
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct users {
    struct write_pointer wp[NUM_DATA];        //write pointer
    struct line_mgmt lm;            //line management
    int id;                         //租户id
    double gc_thres_pcent;          //后台GC阈值百分比
    int gc_thres_lines;             //后台GC阈值数量
    double com_thres_pcent;          //后台COMPACTION阈值百分比
    int com_thres_lines;             //后台COMPACTION阈值数量
    double gc_thres_pcent_high;     //触发强制GC阈值百分比
    int gc_thres_lines_high;        //触发强制GC阈值数量
    
    int weight;                     //租户权重
    int init_lines;                 //租户初始化空间line数量
    uint64_t lpnsize;              //租户应有的LPN
    int64_t tt_time;                    //总共应有的时间
    int64_t re_time;                    //当前周期剩余的时间
    int read_req_persec;      //一秒内总的读请求数量
    int write_req_persec;     //一秒内总的写请求数量
    int gc_read_req_persec;
    int gc_write_req_persec;
    int gc_erase_req_persec;
    int com_read_req_persec;
    int com_write_req_persec;
    int com_erase_req_persec;
    int WAF_write_count;
    int WAF_gc_count;
    int WAF_compaction_count;
    NvmeRequest *user_req;
    struct rte_ring *to_req;
    int com_superblock;
    double WAF_begin;
    double WAF_end;
    double delta_x;

};

// 原始版本
// struct ssd {
//     char *ssdname;
//     struct ssdparams sp;
//     struct ssd_channel *ch;
//     struct ppa *maptbl; /* page level mapping table */
//     uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
//     struct write_pointer wp;    
//     struct line_mgmt lm;

//     /* lockless ring for communication with NVMe IO thread */
//     struct rte_ring **to_ftl;
//     struct rte_ring **to_poller;
//     bool *dataplane_started_ptr;
//     QemuThread ftl_thread;
// };

struct ssd {
    char *ssdname;
    FILE *fp;
    FILE *fp_info;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct line *lines;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    // struct write_pointer wp;
    struct users users[8];  //租户结构体，包含wp和租户号和lm
    // struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
    int users_count;    //租户数量
    uint64_t init_startlpn;  //LPN起始位置
    uint64_t next_ssd_avail_time;
    uint64_t last_print_time_s;
    uint64_t last_measure_time_s;
    int read_req_persec;      //一秒内总的读请求数量
    int write_req_persec;     //一秒内总的写请求数量
    int gc_read_req_persec;
    int gc_write_req_persec;
    int gc_erase_req_persec;
    int com_read_req_persec;
    int com_write_req_persec;
    int com_erase_req_persec;
    bool test_begin;            //开始测试标志位
    bool isgc;                  //是否在做GC
    int ismeasure;
}; 

void ssd_init(FemuCtrl *n);


#define init_file(fp, path) {fp = fopen(path, "w"); fclose(fp);}
#define open_file(fp, path, type) {fp = fopen(path, type);}
#define my_log(fp, format, ...) {fprintf(fp, format, ##__VA_ARGS__); fflush(fp);}
// #define my_log(fp, format, ...) {}
#define output_path "/home/wenyuhong/outputs/text.txt"
#define info_path "/home/wenyuhong/outputs/info.txt"

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
