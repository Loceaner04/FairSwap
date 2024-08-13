#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H
#include <time.h>
#include "../nvme.h"
#define VSSD 20
#define WAOPSHARE 21
#define TIMESHARING 22

/**
 * @file ftl.h
 * @brief ftl相关数据声明
 * 
 */

#include "../nvme.h"

#define INVALID_PPA     (~(0ULL)) //物理地址
#define INVALID_LPN     (~(0ULL)) //逻辑页号
#define UNMAPPED_PPA    (~(0ULL)) 

/**
 * @brief NAND物理时延设置
 * 
 */
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

/**
 * @brief describe a physical page addr
 * 
 */
struct ppa {  
    union {  
        struct {  
            uint64_t blk : BLK_BITS;  
            uint64_t pg  : PG_BITS;  
            uint64_t sec : SEC_BITS;  
            uint64_t pl  : PL_BITS;  
            uint64_t lun : LUN_BITS;  
            uint64_t ch  : CH_BITS;  
            uint64_t rsv : 1; // 通常用于保留位，这里可能是为了对齐或将来使用  
        } g;  
          
        uint64_t ppa; // 作为一个整体的64位物理页地址  
    };  
};
/*
blk: 块号（Block Number），通常用于标识NAND闪存中的块。
pg: 页号（Page Number），用于在块内标识页。
sec: 扇区号（Sector Number），尽管在这个结构体中没有直接使用到扇区（因为通常NAND闪存以页为单位进行读写），但这里可能是为了将来扩展或与其他接口兼容。
pl: 平面号（Plane Number），用于在多平面NAND闪存中标识不同的平面。
lun: 晶圆号（LUN Number），用于在多晶圆SSD中标识不同的晶圆。
ch: 通道号（Channel Number），用于在多通道SSD中标识不同的通道。
rsv: 保留位（Reserved Bit），通常用于对齐或其他目的。
*/

typedef int nand_sec_status_t;

/**
 * @brief 页
sec: 一个指向nand_sec_status_t类型数组的指针，该数组存储了页中每个扇区的状态。
nsecs: 表示页中扇区的数量。
status: 页的状态，可能是一个枚举值，表示页是否有效、无效等。
 * 
 */
struct nand_page {  
    nand_sec_status_t *sec; // 指向存储扇区状态的数组的指针  
    int nsecs; // 扇区数量  
    int status; // 页状态（可能表示页是否有效、无效等）  
};

/*page -> block -> plane -> lun -> channel */

/**
 * @brief 块
pg: 一个指向nand_page类型数组的指针，该数组存储了块中的所有页。
npgs: 块中的页数量。
ipc: 块中无效页的数量。
vpc: 块中有效页的数量。
erase_cnt: 块的擦除计数，通常用于磨损均衡算法，以确定哪个块应该被擦除。
wp: 当前写指针，可能用于页替换算法，决定下一个数据应该写入哪个页。
 * 
 */
struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /**< invalid page count */
    int vpc; /**< valid page count */
    int erase_cnt;
    int wp; /**< current write pointer */
};

/**
 * @brief plane
 * 
 */
struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

/**
 * @brief 晶圆
 * 
 */
struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

/**
 * @brief 通道
 * 
 */
struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

/**
 * @brief ssd参数
 * 
 */
struct ssdparams {
    int secsz;        /**< 扇区大小（以字节为单位） */
    int secs_per_pg;  /**< # 每页扇区数 */
    int pgs_per_blk;  /**< # 每块NAND页数 */
    int blks_per_pl;  /**< # 每个平面的块数 */
    int pls_per_lun;  /**< # 每个 LUN 的平面数（裸片） */
    int luns_per_ch;  /**< # 每个通道的 LUN 数 */
    int nchs;         /**< # SSD 中的通道数 */

    int pg_rd_lat;    /**< NAND页面读取延迟（纳秒） */
    int pg_wr_lat;    /**< NAND页面程序延迟（以纳秒为单位） */
    int blk_er_lat;   /**< NAND模块以纳秒为单位擦除延迟 */
    int ch_xfer_lat;  /**< 一页的信道传输延迟（以纳秒为单位），这定义了信道带宽 */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay; 

    /* 以下是所有计算值 */
    int secs_per_blk; /**< # of sectors per block */
    int secs_per_pl;  /**< # of sectors per plane */
    int secs_per_lun; /**< # of sectors per LUN */
    int secs_per_ch;  /**< # of sectors per channel */
    int tt_secs;      /**< # of sectors in the SSD */

    int pgs_per_pl;   /**< # of pages per plane */
    int pgs_per_lun;  /**< # of pages per LUN (Die) */
    int pgs_per_ch;   /**< # of pages per channel */
    int tt_pgs;       /**< total # of pages in the SSD */

    int blks_per_lun; /**< # of blocks per LUN */
    int blks_per_ch;  /**< # of blocks per channel */
    int tt_blks;      /**< total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /**< # of planes per channel */
    int tt_pls;       /**< total # of planes in the SSD */

    int tt_luns;      /**< total # of LUNs in the SSD */
};

/**
 * @brief line
 * 
 */
typedef struct line {
    int id;  /**< 行 ID，与对应的块 ID 相同 */
    int ipc; /**< 无效的页数 in this line */
    int vpc; /**< 有效页数 in this line */
    QTAILQ_ENTRY(line) entry; /**< 在任一 {free，victim，full} 列表中 */
    /**< position in the priority queue for victim lines */
    size_t pos;
} line;

/**
 * @brief wp: record next write addr
 * 
 */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

/**
 * @brief 记录各种line的列表和数量
 * FTL 主要的数据结构有：
Page 粒度映射表 struct ssd.maptbl。直接使用数组保存 LPN->PPN 的映射
反向映射表 struct ssd.rmap。使用数组保存 PPN->LPN 的映射
用于 FTL 的空闲 line 链表 struct line_mgmt.free_line_list
用于 FTL 的全部包含有效数据的 line 链表 struct line_mgmt.full_line_list
用于 FTL 的包含部分有效数据的 line 优先级队列 struct line_mgmt.victim_line_pq
当前写位置指针 struct write_pointer，指向当前的 line 以及 line 内的具体位置
 */
struct line_mgmt {
    struct line *lines;
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
    int64_t stime; /**< Coperd: request arrival time */
};

/**
 * @brief 模拟一个ssd
 * 
 */
struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl;             ///< 页映射表
    uint64_t *rmap;                 ///< 反向映射表, assume it's stored in OOB
    struct write_pointer wp;
    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;       ///< 无锁ring队列，用于接收nvme下传的req
    struct rte_ring **to_poller;    ///< 存放完成的req，交给nvme线程
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void ssd_init(FemuCtrl *n);

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
