#include "ftl.h"
#include <stdint.h>
#include <stdio.h>

// void myprint(char *format, ...)
// {
//     FILE *fp = fopen("/home/wenyuhong/outputs/text.txt", "a+");
//     fprintf(fp, format, ...);
//     fclose(fp);
//     fflush(fp);
//     return;
// }


//#define FEMU_DEBUG_FTL

/*
*   11.2 改动
*   ssd结构体中line management、wpp改动至users结构体下;
*   get_new_page\ssd_advance_write_poiter函数增加变量user_id;
*   sp结构体中的各gc阈值放置在ssd中，初始化需要改动
*/

//lpn2userid
static void ssd_init_usertime(struct ssd *ssd) {
    int64_t ttime = 1e9;
    int tt_weight = 0;
    for (int user_id = 0; user_id < ssd->users_count; user_id++) {
        tt_weight += ssd->users[user_id].weight;
    }
    for (int user_id = 0; user_id < ssd->users_count; user_id++) {
        ssd->users[user_id].tt_time = ttime / tt_weight * ssd->users[user_id].weight * ssd->sp.luns_per_ch * ssd->sp.nchs;
        ssd->users[user_id].re_time = ssd->users[user_id].tt_time;
        ssd->users[user_id].to_req = femu_ring_create(FEMU_RING_TYPE_SP_SC, FEMU_MAX_INF_REQS);
        ssd->users[user_id].user_req = NULL;
        my_log(ssd->fp, "users:%d, tt_time = %ld\n", user_id, ssd->users[user_id].tt_time);
    }
}

static void printf_info(struct ssd *ssd) {
    ssd->next_ssd_avail_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int time_s = ssd->next_ssd_avail_time / 1e9;
    if(time_s > ssd->last_print_time_s && ssd->test_begin) {
        ssd->last_print_time_s = time_s;
        if (ssd->read_req_persec+ssd->write_req_persec+ssd->gc_read_req_persec+ssd->gc_write_req_persec+ssd->gc_erase_req_persec!=0) {
            my_log(ssd->fp_info, "time:%d, tt_IOs:(%d,%d), \
            tt_GC_IOs:(%d,%d,%d)\n", time_s, ssd->read_req_persec, ssd->write_req_persec, ssd->gc_read_req_persec, ssd->gc_write_req_persec, ssd->gc_erase_req_persec);
            for(int user_id = 0; user_id < ssd->users_count; user_id++) {
                my_log(ssd->fp_info, "\tuser_id:%d, IOs:(%d,%d), GC_IOs:(%d,%d,%d), re_time = %ld\n", user_id, ssd->users[user_id].read_req_persec,ssd->users[user_id].write_req_persec, ssd->users[user_id].gc_read_req_persec, ssd->users[user_id].gc_write_req_persec, ssd->users[user_id].gc_erase_req_persec, ssd->users[user_id].re_time);
                ssd->users[user_id].read_req_persec = ssd->users[user_id].write_req_persec = ssd->users[user_id].gc_read_req_persec = ssd->users[user_id].gc_write_req_persec = ssd->users[user_id].gc_erase_req_persec = 0;
                ssd->users[user_id].re_time = ssd->users[user_id].tt_time;
            }
        }
        ssd->read_req_persec = ssd->write_req_persec = ssd->gc_read_req_persec = ssd->gc_write_req_persec = ssd->gc_erase_req_persec = 0;    
    }
}

static int lpn2userid(struct ssd *ssd, uint64_t start_lpn, uint64_t end_lpn)
{
    if (!start_lpn && !end_lpn) {
        return 0;
    }
    if (start_lpn < ssd->init_startlpn && end_lpn < ssd->init_startlpn) {
        return 0;
    }
    uint64_t user_startlpn = ssd->init_startlpn;
    for(int user_id = 0; user_id < ssd->users_count; user_id++) {
        uint64_t user_endlpn = user_startlpn + ssd->users[user_id].lpnsize - 1;
        if (start_lpn >= user_startlpn && end_lpn <= user_endlpn) {
            return user_id;
        }
        user_startlpn = user_endlpn + 1;
    }
    my_log(ssd->fp,"lpn2userid maybe error, start_lpn = %lu, end_lpn = %lu\n", start_lpn, end_lpn);
    user_startlpn = ssd->init_startlpn;
    for(int user_id = 0; user_id < ssd->users_count; user_id++) {
        uint64_t user_endlpn = user_startlpn + ssd->users[user_id].lpnsize - 1;
        my_log(ssd->fp,"user_id = %d, start_lpn = %lu, end_lpn = %lu\n", user_id, user_startlpn, user_endlpn);
        user_startlpn = user_endlpn + 1;
    }
    return -1;
}

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd, int user_id)
{
    return (ssd->users[user_id].lm.free_line_cnt <= ssd->users[user_id].gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd, int user_id)
{
    return (ssd->users[user_id].lm.free_line_cnt <= ssd->users[user_id].gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    ssd->lines = g_malloc0(sizeof(struct line) * spp->tt_lines);
    struct line *line_global;
    int n = 0;
    int tt_weight = 0;
    for (int i = 0; i < spp->tt_lines; i++) //全局初始化
    {
        line_global = &ssd->lines[i];
        line_global->id = i;
        line_global->ipc = 0;
        line_global->vpc = 0;
        line_global->pos = 0;
    }
    for(int user_id = 0; user_id < ssd->users_count; user_id++){
        struct line_mgmt *lm = &ssd->users[user_id].lm;
        struct line *line;

        // lm->tt_lines = spp->blks_per_pl; 
        lm->tt_lines = ssd->users[user_id].init_lines;
        // ftl_assert(lm->tt_lines == spp->tt_lines);
        // lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

        QTAILQ_INIT(&lm->free_line_list);
        lm->victim_line_pq = pqueue_init(lm->tt_lines, victim_line_cmp_pri,
                victim_line_get_pri, victim_line_set_pri,
                victim_line_get_pos, victim_line_set_pos);
        QTAILQ_INIT(&lm->full_line_list);
        lm->free_line_cnt = 0;
        // 原有不同租户的line号会重复
        for (int i = 0; i < lm->tt_lines; i++) {
            line = &ssd->lines[n++];
            ftl_assert(n < spp->tt_lines);
            // line->id = i;
            // line->ipc = 0;
            // line->vpc = 0;
            // line->pos = 0;
            /* initialize all the lines as free lines */
            QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
            lm->free_line_cnt++;
            //my_log(ssd->fp, "user_id:%d line_ID = %d all line = %d\n", user_id, line->id, spp->tt_lines);
        }
        ftl_assert(lm->free_line_cnt == lm->tt_lines);
        lm->victim_line_cnt = 0;
        lm->full_line_cnt = 0;
        tt_weight += ssd->users[user_id].weight;
        //my_log(ssd->fp, "user_id:%d info: tt_lines = %d, space = %d, LPN = %lu, free_line_cnt = %d, victim_line_cnt = %d, full_line_cnt = %d\n", user_id, lm->tt_lines, lm->tt_lines * 64, ssd->users[user_id].lpnsize, lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt);
    }

    //以下为分配剩余的OP空间
    int op_space_cnt = spp->tt_lines - n;
    //my_log(ssd->fp, "op_space_cnt = spp->tt_lines - n = %d\n", op_space_cnt);
    for(int user_id = 0; user_id < ssd->users_count; user_id++) {
        struct line *line;
        int lines_cnt = ssd->users[user_id].weight * op_space_cnt / tt_weight;  //分配的OP空间大小，向下取整
        struct line_mgmt *lm = &ssd->users[user_id].lm;
        for(int i = 0; i < lines_cnt; i++) {
            line = &ssd->lines[n++];
            ftl_assert(n < spp->tt_lines);
            QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
            lm->free_line_cnt++;
            lm->tt_lines++;
            //my_log(ssd->fp, "user_id:%d line_ID = %d\n", user_id, line->id);
        }
        ssd->users[user_id].gc_thres_lines = (int)((1 - ssd->users[user_id].gc_thres_pcent) * lm->tt_lines);    //更新GC百分比阈值
        ssd->users[user_id].gc_thres_lines_high = (int)((1 - ssd->users[user_id].gc_thres_pcent_high) * lm->tt_lines);  //更新GC百分比阈值
        //my_log(ssd->fp, "user_id:%d info: lines_cnt = %d tt_lines = %d, space = %d, free_line_cnt = %d, victim_line_cnt = %d, full_line_cnt = %d\n", user_id, lines_cnt, lm->tt_lines, lm->tt_lines * 64, lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt);
    }
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    for (int user_id = 0; user_id < ssd->users_count; user_id++) {
        struct write_pointer *wpp = &ssd->users[user_id].wp;
        struct line_mgmt *lm = &ssd->users[user_id].lm;
        struct line *curline = NULL;

        curline = QTAILQ_FIRST(&lm->free_line_list);
        
        QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
        lm->free_line_cnt--;

        /* wpp->curline is always our next-to-write super-block */
        wpp->curline = curline;
        wpp->ch = 0;
        wpp->lun = 0;
        wpp->pg = 0;
        wpp->blk = curline->id;
        wpp->pl = 0;
    }
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd, int user_id)
{
    struct line_mgmt *lm = &ssd->users[user_id].lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, int user_id)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->users[user_id].wp;
    struct line_mgmt *lm = &ssd->users[user_id].lm;

    check_addr(wpp->ch, spp->nchs);

    wpp->curline->T_written = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);   //更新line的T_written
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd,user_id);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd,int user_id)
{
    struct write_pointer *wpp = &ssd->users[user_id].wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n, struct ssd *ssd)
{
    //my_log(ssd->fp, "success to ssd_init_params\n");

    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;
    ssd->users_count = n->bb_params.users_count;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    for (int user_id = 0; user_id < ssd->users_count; user_id++)
    {
        ssd->users[user_id].gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0; //todo
        //ssd->users[user_id].gc_thres_lines = (int)((1 - ssd->users[user_id].gc_thres_pcent) * spp->tt_lines);
        ssd->users[user_id].gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
        //ssd->users[user_id].gc_thres_lines_high = (int)((1 - ssd->users[user_id].gc_thres_pcent_high) * spp->tt_lines);
        // ssd->users[user_id].enable_gc_delay = true;
        ssd->users[user_id].weight = n->bb_params.user_init[user_id].weight;
        ssd->users[user_id].init_lines = n->bb_params.user_init[user_id].space_mb  / (spp->secs_per_line * spp->secsz / 1024 / 1024 ); 
        ssd->users[user_id].lpnsize = (uint64_t) n->bb_params.user_init[user_id].space_mb / 4 * 1024;
    }

    // spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0;
    // spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    // spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0;
    // spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;
    //my_log(ssd->fp, "users_count = %d \n", ssd->users_count);
    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    init_file(ssd->fp, output_path);
    open_file(ssd->fp, output_path, "a+");
    //my_log(ssd->fp, "success to ssd_init\n");
    init_file(ssd->fp_info, info_path);
    open_file(ssd->fp_info, info_path, "a+");

    ssd_init_params(spp, n, ssd);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    ssd_init_usertime(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);
    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd, int user_id)
{
    int c = ncmd->cmd;
    // uint64_t cmd_stime = (ncmd->stime == 0) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t cmd_stime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    // int curtime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1e9;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->users[user_id].re_time = ssd->users[user_id].re_time - spp->pg_rd_lat;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->users[user_id].re_time = ssd->users[user_id].re_time - spp->pg_wr_lat;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        ssd->users[user_id].re_time = ssd->users[user_id].re_time - spp->blk_er_lat;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }
    // int luntime = lun->next_lun_avail_time / 1e9;
    //my_log(ssd->fp, "curtime=%d, luntime=%d\n", curtime, luntime);
    // if (lun->next_lun_avail_time > ssd->next_ssd_avail_time) {
    //     ssd->next_ssd_avail_time = lun->next_lun_avail_time;
    // }
    // printf_info(ssd);
    
    //my_log(ssd->fp_info, "start time = %ld, lat = %ld, end time = %ld, writelpn:%d\n", ssd->users[user_id].re_time, lat, ssd->users[user_id].re_time - lat, ssd->users[user_id].write_req_persec);
    printf_info(ssd);
    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa, int user_id)
{
    struct line_mgmt *lm = &ssd->users[user_id].lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    //my_log(ssd->fp, "line vpc = %d, line ipc = %d, spp->pgs_per_line = %d\n", line->vpc, line->ipc, spp->pgs_per_line);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        //my_log(ssd->fp, "line->pos = %lu\n", line->pos);
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    // my_log(ssd->fp, "Now writing line id = %d\n", line->id);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa, int user_id)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        if(ssd->test_begin)
        {
            ssd->gc_read_req_persec++;
            ssd->users[user_id].gc_read_req_persec++;
        }
        ssd_advance_status(ssd, ppa, &gcr, user_id);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int user_id)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, user_id);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, user_id);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        if(ssd->test_begin) {
            ssd->gc_write_req_persec++;
            ssd->users[user_id].gc_write_req_persec++;
        }
        ssd_advance_status(ssd, &new_ppa, &gcw, user_id);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force, int user_id)
{
    struct line_mgmt *lm = &ssd->users[user_id].lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    //pqueue_pop(lm->victim_line_pq);
    //lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static bool clean_one_block(struct ssd *ssd, struct ppa *ppa, int user_id)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    // struct line *curline = get_line(ssd, ppa);
    // struct nand_block *curblk = get_blk(ssd, ppa);
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa, user_id);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa, user_id);   //改动
            cnt++;
            mark_page_invalid(ssd, ppa, user_id);
        }
        
        if (ssd->users[user_id].re_time < 0) {
            return false;
        }
        
    }
    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    return true;
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa, int user_id)
{
    struct line_mgmt *lm = &ssd->users[user_id].lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force, int user_id)
{
    //my_log(ssd->fp, "GC Starting...\nuser id:%d, full line = %d, victim line = %d, free line = %d\n", user_id, ssd->users[user_id].lm.full_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.free_line_cnt);
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    bool is_finish = true;
    struct line_mgmt *lm = &ssd->users[user_id].lm;

    victim_line = select_victim_line(ssd, force, user_id);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    // my_log(ssd->fp, "GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
    //           victim_line->ipc, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt,
    //           ssd->users[user_id].lm.free_line_cnt);
    // ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
    //           victim_line->ipc, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt,
    //           ssd->users[user_id].lm.free_line_cnt);
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            is_finish = clean_one_block(ssd, &ppa, user_id);
            lunp = get_lun(ssd, &ppa);
            
            if (is_finish == false)
            {
                //pqueue_insert(lm->victim_line_pq, victim_line);
                //lm->victim_line_cnt++;
                //pqueue_change_priority(lm->victim_line_pq, victim_line->vpc, victim_line);
                return 0;
            }

            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                if(ssd->test_begin) {
                    ssd->gc_erase_req_persec++;
                    ssd->users[user_id].gc_erase_req_persec++;
                }
                ssd_advance_status(ssd, &ppa, &gce, user_id);
            }
            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }
    pqueue_remove(lm->victim_line_pq, victim_line);
    lm->victim_line_cnt--;

    /* update line status */
    mark_line_free(ssd, &ppa, user_id);
    //my_log(ssd->fp, "user id:%d, full line = %d, victim line = %d, free line = %d\nGC Ending......\n", user_id, ssd->users[user_id].lm.full_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.free_line_cnt);
    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;       //一个sector
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    //my_log(ssd->fp, "SSD_READING......   START_LPN = %lu, END_LPN = %lu\n", start_lpn, end_lpn);
    int user_id;
    if (ssd->test_begin == false) {
        user_id = 0;
    }
    else {
        user_id = lpn2userid(ssd, start_lpn, end_lpn);
    }
    if (user_id == -1)
    {
        ftl_log("ssd_read err!\n");
    }
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        if(ssd->test_begin) {
            ssd->read_req_persec++;
            ssd->users[user_id].read_req_persec++;
        }
        sublat = ssd_advance_status(ssd, &ppa, &srd, user_id);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
        
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    if(!ssd->init_startlpn && start_lpn != 0) {
        ssd->init_startlpn = start_lpn;
        my_log(ssd->fp,"init_startlpn = %lu\n", ssd->init_startlpn);
    }
    if (ssd->init_startlpn != 0) {
        ssd->test_begin = true;
    }
    int user_id = lpn2userid(ssd, start_lpn, end_lpn);
    //my_log(ssd->fp, "SSD_WRINTING......   USER_ID = %d, START_LPN = %lu, LEN = %lu, END_LPN = %lu, INIT_START_LPN = %lu\n", user_id, start_lpn, end_lpn-start_lpn, end_lpn, ssd->init_startlpn);
    if (user_id == -1)
    {
        ftl_log("ssd_write err!\n");
    }

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd, user_id)) {
        /* perform GC here until !should_gc(ssd) */
        //my_log(ssd->fp, "Doing High GC Now..... USER_ID = %d\n", user_id);
        my_log(ssd->fp, "/***************HIGH GC START*******************/\n");
        my_log(ssd->fp,"userid = %d, free line cnt = %d, victim line cnt = %d, full line cnt = %d\n", user_id, ssd->users[user_id].lm.free_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt);
        r = do_gc(ssd, true, user_id);
        my_log(ssd->fp,"userid = %d, free line cnt = %d, victim line cnt = %d, full line cnt = %d\n", user_id, ssd->users[user_id].lm.free_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt);
        if (r == -1)
            break;
        printf_info(ssd);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            //my_log(ssd->fp, "mapped_ppa\n");
            mark_page_invalid(ssd, &ppa, user_id);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);

            //置无效
        }

        /* new write */
        ppa = get_new_page(ssd, user_id); //update by 11.2
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);
        if(ssd->test_begin) {
            ssd->write_req_persec++;
            ssd->users[user_id].write_req_persec++;
        }
        /* need to advance the write pointer here */

        ssd_advance_write_pointer(ssd, user_id);  //update by 11.2

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr, user_id);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

// static void *ftl_thread(void *arg)
// {
//     FemuCtrl *n = (FemuCtrl *)arg;
//     struct ssd *ssd = n->ssd;
//     NvmeRequest *req = NULL;
//     uint64_t lat = 0;
//     int rc;
//     int i;

//     while (!*(ssd->dataplane_started_ptr)) {
//         usleep(100000);
//     }

//     /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
//     ssd->to_ftl = n->to_ftl;
//     ssd->to_poller = n->to_poller;

//     while (1) {
//         printf_info(ssd);
//         for (i = 1; i <= n->nr_pollers; i++) {
//             printf_info(ssd);
//             if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
//                 continue;

//             rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
//             if (rc != 1) {
//                 printf("FEMU: FTL to_ftl dequeue failed\n");
//             }
//             ftl_assert(req);
//             switch (req->cmd.opcode) {
//             case NVME_CMD_WRITE:
//                 lat = ssd_write(ssd, req);
//                 break;
//             case NVME_CMD_READ:
//                 lat = ssd_read(ssd, req);
//                 break;
//             case NVME_CMD_DSM:
//                 lat = 0;
//                 break;
//             default:
//                 //ftl_err("FTL received unkown request type, ERROR\n");
//                 ;
//             }

//             req->reqlat = lat;
//             req->expire_time += lat;

//             rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
//             if (rc != 1) {
//                 ftl_err("FTL to_poller enqueue failed\n");
//             }

//             /* clean one line if needed (in the background) */
//             // if (should_gc(ssd)) {
//             //     do_gc(ssd, false);
//             // }

//             for(int i = 0; i < ssd->users_count; i++){
//                 if(should_gc(ssd, i)){
//                     //my_log(ssd->fp, "Doing Low GC Now..... USER_ID = %d\n", i);
//                     do_gc(ssd, false, i);
//                 }
//             }
//         }
//     }
//     return NULL;
// }

static void ssd_add_queue(struct ssd *ssd, NvmeRequest *req) {
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    int user_id;
    if (ssd->test_begin) {
        user_id = lpn2userid(ssd, start_lpn, end_lpn);
        if (user_id == -1)
        {
            ftl_log("ssd_add_queue err!\n");
        }
    }
    else {
        user_id = 0;
    }
    int rc;
    rc = femu_ring_enqueue(ssd->users[user_id].to_req, (void *)&req, 1);
    if (rc != 1) {
        printf("FEMU: FTL to_req[%d] add_queue dequeue failed\n", user_id);
    }
}

static void ssd_do_queue(struct ssd *ssd) {
    int rc;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int64_t ttime = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for (int user_id = 0; user_id < ssd->users_count; user_id++)
    {
        if (ssd->users[user_id].re_time < 0) {
            continue;
        }
        if (/*!ssd->users[user_id].to_req ||*/ ssd->users[user_id].user_req == NULL && !femu_ring_count(ssd->users[user_id].to_req)) {
            continue;
        }
        if(ssd->users[user_id].user_req != NULL) {
            req = ssd->users[user_id].user_req;
            if (req->stime > ttime) {
                req = NULL;
                continue;
            }
            else {
                ssd->users[user_id].user_req = NULL;
            }
        }
        else {
            rc = femu_ring_dequeue(ssd->users[user_id].to_req, (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_req[%d] do_queue dequeue failed\n", user_id);
            }
            if (req->stime > ttime) {
                ssd->users[user_id].user_req = req;
                req = NULL;
                continue;
            }
        }
        //my_log(ssd->fp, "start: user_id = %d, count_req = %lu\n", user_id, femu_ring_count(ssd->users[user_id].to_req));
        
        ftl_assert(req);
        switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                //my_log(ssd->fp, "NVME_CMD_WRITE\n");
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                //my_log(ssd->fp, "NVME_CMD_READ\n");
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
        }
       
        req->reqlat = lat;
        req->expire_time += lat;
        /*
        if (lat != 0) {
            if (req->cmd_opcode == NVME_CMD_READ) {
                my_log(ssd->fp_info, "UESRID= %d, READ: lba:%lu, stime:%ld, lat:%ld, expire_time:%ld\n", user_id, req->slba, req->stime, req->reqlat, req->expire_time);
            }
            else if (req->cmd_opcode == NVME_CMD_WRITE) {
                my_log(ssd->fp_info, "UESRID= %d, WRITE: lba:%lu, stime:%ld, lat:%ld, expire_time:%ld\n", user_id, req->slba, req->stime, req->reqlat, req->expire_time);
            }
            else {
                my_log(ssd->fp_info, "UESRID= %d, OTHERS: lba:%lu, stime:%ld, lat:%ld, expire_time:%ld\n", user_id, req->slba, req->stime, req->reqlat, req->expire_time);
            }
        }*/
        rc = femu_ring_enqueue(ssd->to_poller[1], (void *)&req, 1);
        req = NULL;
        //my_log(ssd->fp, "user_id = %d, count_req = %lu....end\n", user_id, femu_ring_count(ssd->users[user_id].to_req));
        if (rc != 1) {
            ftl_err("FTL to_poller[%d] enqueue failed\n", user_id);
        }
    }
}
static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    //uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        printf_info(ssd);
        ssd_do_queue(ssd);
        for (i = 1; i <= n->nr_pollers; i++) {
            printf_info(ssd);
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            if (req->cmd.opcode == NVME_CMD_WRITE || req->cmd.opcode == NVME_CMD_READ) {
                ssd_add_queue(ssd, req);
            }
            else {
                req->reqlat = 0;
                req->expire_time += 0;
                rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
                if (rc != 1) {
                    ftl_err("FTL to_poller enqueue failed\n");
                }    
                continue;
            }
            ssd_do_queue(ssd);
            for(int user_id = 0; user_id < ssd->users_count; user_id++){
                if(should_gc(ssd, user_id)){
                    my_log(ssd->fp, "/***************LOW GC START*******************/\n");
                    my_log(ssd->fp,"userid = %d, free line cnt = %d, victim line cnt = %d, full line cnt = %d\n", user_id, ssd->users[user_id].lm.free_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt);
                    do_gc(ssd, false, user_id);
                    my_log(ssd->fp,"userid = %d, free line cnt = %d, victim line cnt = %d, full line cnt = %d\n", user_id, ssd->users[user_id].lm.free_line_cnt, ssd->users[user_id].lm.victim_line_cnt, ssd->users[user_id].lm.full_line_cnt);
                    //my_log(ssd->fp, "Doing Low GC Now..... USER_ID = %d\n", i);
                }
            }
        }
    }
    return NULL;
}
