/* vere/pier.c
*/
#include <ent.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <setjmp.h>
#include <gmp.h>
#include <sigsegv.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <uv.h>
#include <errno.h>
#include <ncurses/curses.h>
#include <termios.h>
#include <ncurses/term.h>

#include "all.h"
#include "vere/vere.h"

#undef VERBOSE_EVENTS

  /*    event handling proceeds on a single path. across both the
  **    child worker process (serf) and parent i/o process (king).
  **    state transitions are as follows:
  **
  **        generated               (event numbered and queued)
  **        dispatched              (sent to worker)
  **        computed                (completed by worker)
  **        commit requested        (sent to storage subsystem)
  **        commit complete         (king notified)
  **        released                (output actions allowed)
  **
  **    we dispatch one event at a time to the worker.  we don't do
  **    anything in parallel.
  **
  **    the sanity constraints that constrain this path:
  **
  **        - an event can't request a commit until it's computed.
  **        - an event can't be released until it, and all events
  **          preceding it, are computed and committed.
  **
  **    event numbers are uint64 (c3_d) which start with 1.  we order
  **    events as we receive them.
  **
  **    events are executed in order by the working process, and
  **    (at present) committed in strict order.
  **
  **    the result of computing an event can be completion (in which
  **    case we go directly to commit) or replacement (in which we
  **    replace the input event with a different event).
  **
  **    after crash recovery, events committed but not in the snapshot
  **    (the state of the serf) are replayed (re-computed), but their
  **    output effects are ignored. it is possible that effects of
  **    (only the last of ?) these events are not completely released to
  **    the outside world -- but they should never be released more than once.
  **
  **    XX analyze replay more comprehensively
  */

static void _pier_apply(u3_pier* pir_u);
static void _pier_boot_complete(u3_pier* pir_u);
static void _pier_exit_done(u3_pier* pir_u);
static void _pier_loop_exit(u3_pier* pir_u);
static void _pier_work_save(u3_pier* pir_u);

/* _pier_disk_bail(): bail from disk i/o.
*/
static void
_pier_disk_bail(void* vod_p, const c3_c* err_c)
{
  // u3_writ* wit_u = vod_p;

  fprintf(stderr, "disk error: %s\r\n", err_c);
}

/* _pier_work_bail(): handle subprocess error.
*/
static void
_pier_work_bail(void*       vod_p,
                const c3_c* err_c)
{
  fprintf(stderr, "pier: work error: %s\r\n", err_c);
}

/* _pier_save_boot_complete(): commit complete.
*/
static void
_pier_save_boot_complete(void* vod_p)
{
  //  no-op, callback required by u3_foil_append()
  //
}

/* _pier_save_boot(): save boot metadata.
*/
static void
_pier_save_boot(u3_pier* pir_u, u3_atom mat)
{
  //  XX deduplicate with _pier_disk_commit_request
  //
  c3_d  len_d = u3r_met(6, mat);
  c3_d* buf_d = c3_malloc(8 * len_d);

  u3r_chubs(0, len_d, buf_d, mat);

  u3_foil_append(_pier_save_boot_complete,
                 (void*)0,
                 pir_u->log_u->fol_u,
                 buf_d,
                 len_d);
}

/* _pier_work_boot(): prepare serf boot.
*/
static void
_pier_work_boot(u3_pier* pir_u, c3_o sav_o)
{
  u3_lord* god_u = pir_u->god_u;

  c3_assert( 0 != pir_u->lif_d );

  u3_noun who = u3i_chubs(2, pir_u->who_d);
  u3_noun len = u3i_chubs(1, &pir_u->lif_d);
  u3_noun msg = u3nq(c3__boot, who, pir_u->fak_o, len);
  u3_atom mat = u3ke_jam(msg);

  if ( c3y == sav_o ) {
    _pier_save_boot(pir_u, u3k(mat));
  }

  u3_newt_write(&god_u->inn_u, mat, 0);
}

/* _pier_disk_shutdown(): close the log.
*/
static void
_pier_disk_shutdown(u3_pier* pir_u)
{
}

/* _pier_work_shutdown(): stop the worker process.
*/
static void
_pier_work_shutdown(u3_pier* pir_u)
{
  u3_lord* god_u = pir_u->god_u;

  u3_newt_write(&god_u->inn_u, u3ke_jam(u3nc(c3__exit, 0)), 0);
}

/* _pier_insert(): insert raw event.
*/
static void
_pier_insert(u3_pier* pir_u,
             c3_l     msc_l,
             u3_noun  job)
{
  u3_writ* wit_u = c3_calloc(sizeof(u3_writ));
  wit_u->pir_u = pir_u;

  wit_u->evt_d = pir_u->gen_d;
  pir_u->gen_d++;

  wit_u->msc_l = msc_l;

  wit_u->job = job;

  if ( !pir_u->ent_u ) {
    c3_assert(!pir_u->ext_u);

    pir_u->ent_u = pir_u->ext_u = wit_u;
  }
  else {
    pir_u->ent_u->nex_u = wit_u;
    pir_u->ent_u = wit_u;
  }
}

/* _pier_insert_ovum(): insert raw ovum.
*/
static void
_pier_insert_ovum(u3_pier* pir_u,
                  c3_l     msc_l,
                  u3_noun  ovo)
{
  u3_noun        now;
  struct timeval tim_tv;

  gettimeofday(&tim_tv, 0);
  now = u3_time_in_tv(&tim_tv);

  _pier_insert(pir_u, msc_l, u3nc(now, ovo));
}

/* _pier_disk_commit_complete(): commit complete.
*/
static void
_pier_disk_commit_complete(void* vod_p)
{
  u3_writ* wit_u = vod_p;
  u3_pier* pir_u = wit_u->pir_u;
  u3_disk* log_u = pir_u->log_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): commit: complete\r\n", wit_u->evt_d);
#endif

  /* advance commit counter
  */
  {
    c3_assert(wit_u->evt_d == log_u->moc_d);
    c3_assert(wit_u->evt_d == (1ULL + log_u->com_d));
    log_u->com_d += 1ULL;
  }

  _pier_apply(pir_u);
}

/* _pier_disk_commit_request(): start commit.
*/
static void
_pier_disk_commit_request(u3_writ* wit_u)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_disk* log_u = pir_u->log_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): commit: request\r\n", wit_u->evt_d);
#endif

  /* append to logfile
  */
  {
    c3_d  len_d = u3r_met(6, wit_u->mat);
    c3_d* buf_d = c3_malloc(8 * len_d);

    u3r_chubs(0, len_d, buf_d, wit_u->mat);
    u3_foil_append(_pier_disk_commit_complete,
                   wit_u,
                   log_u->fol_u,
                   buf_d,
                   len_d);
  }

  /* advance commit-request counter
  */
  {
    c3_assert(wit_u->evt_d == (1ULL + log_u->moc_d));
    log_u->moc_d += 1ULL;
  }
}

/* _pier_writ_unlink(): unlink writ from queue.
*/
static void
_pier_writ_unlink(u3_writ* wit_u)
{
  u3_pier* pir_u = wit_u->pir_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): delete\r\n", wit_u->evt_d);
#endif

  pir_u->ext_u = wit_u->nex_u;

  if ( wit_u == pir_u->ent_u ) {
    c3_assert(pir_u->ext_u == 0);
    pir_u->ent_u = 0;
  }
}

/* _pier_writ_dispose(): dispose of writ.
*/
static void
_pier_writ_dispose(u3_writ* wit_u)
{
  /* free contents
  */
  u3z(wit_u->job);
  u3z(wit_u->mat);
  u3z(wit_u->act);

  c3_free(wit_u);
}

/* _pier_work_release(): apply side effects.
*/
static void
_pier_work_release(u3_writ* wit_u)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_lord* god_u = pir_u->god_u;
  u3_noun    vir = wit_u->act;

  if ( u3_psat_pace == pir_u->sat_e ) {
    fputc('.', stderr);
  }
  else {
#ifdef VERBOSE_EVENTS
    fprintf(stderr, "pier: (%" PRIu64 "): compute: release\r\n", wit_u->evt_d);
#endif

    //  advance release counter
    //
    {
      c3_assert(wit_u->evt_d == (1ULL + god_u->rel_d));
      god_u->rel_d += 1ULL;
    }

    //  apply actions
    //
    while ( u3_nul != vir ) {
      u3_noun ovo, nex;
      u3x_cell(vir, &ovo, &nex);

      u3_reck_kick(pir_u, u3k(ovo));
      vir = nex;
    }
  }

  //  if we have completed the boot sequence, activate system events.
  //
  if ( wit_u->evt_d == pir_u->but_d ) {
    _pier_boot_complete(pir_u);
  }

  //  take snapshot, if requested (and awaiting the commit of this event)
  //
  {
    u3_save* sav_u = pir_u->sav_u;

    if ( (sav_u->req_d > sav_u->dun_d) &&
         (wit_u->evt_d == sav_u->req_d) )
    {
      _pier_work_save(pir_u);
    }
  }
}

/* _pier_work_build(): build atomic action.
*/
static void
_pier_work_build(u3_writ* wit_u)
{
  /* marshal into atom
  */
  if ( 0 == wit_u->mat ) {
    c3_assert(0 != wit_u->job);

    wit_u->mat = u3ke_jam(u3nq(c3__work,
                               u3i_chubs(1, &wit_u->evt_d),
                               wit_u->mug_l,
                               u3k(wit_u->job)));
  }
}

/* _pier_work_send(): send to worker.
*/
static void
_pier_work_send(u3_writ* wit_u)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_lord* god_u = pir_u->god_u;

  c3_assert(0 != wit_u->mat);

  u3_newt_write(&god_u->inn_u, u3k(wit_u->mat), wit_u);
}

/* _pier_work_save(): tell worker to save checkpoint.
*/
static void
_pier_work_save(u3_pier* pir_u)
{
  u3_lord* god_u = pir_u->god_u;
  u3_disk* log_u = pir_u->log_u;
  u3_save* sav_u = pir_u->sav_u;

  c3_assert( god_u->dun_d == sav_u->req_d );
  c3_assert( log_u->com_d >= god_u->dun_d );

  {
    u3_noun mat = u3ke_jam(u3nc(c3__save, u3i_chubs(1, &god_u->dun_d)));
    u3_newt_write(&god_u->inn_u, mat, 0);

    //  XX wait on some report of success before updating?
    //
    sav_u->dun_d = sav_u->req_d;
  }

  //  if we're gracefully shutting down, do so now
  //
  if ( u3_psat_done == pir_u->sat_e ) {
    _pier_exit_done(pir_u);
  }
}

/* u3_pier_snap(): request snapshot
*/
void
u3_pier_snap(u3_pier* pir_u)
{
  u3_lord* god_u = pir_u->god_u;
  u3_disk* log_u = pir_u->log_u;
  u3_save* sav_u = pir_u->sav_u;

  c3_d top_d = c3_max(god_u->sen_d, god_u->dun_d);

  //  no-op if there are no un-snapshot'ed events
  //
  if ( top_d > sav_u->dun_d ) {
    sav_u->req_d = top_d;

    //  save eagerly if all computed events are already committed
    //
    if ( log_u->com_d >= top_d ) {
      _pier_work_save(pir_u);
    }
  }
  //  if we're gracefully shutting down, do so now
  //
  else if ( u3_psat_done == pir_u->sat_e ) {
    _pier_exit_done(pir_u);
  }
}

/* _pier_work_complete(): worker reported completion.
*/
static void
_pier_work_complete(u3_writ* wit_u,
                    c3_l     mug_l,
                    u3_noun  act)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_lord* god_u = pir_u->god_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): compute: complete\r\n", wit_u->evt_d);
#endif

  god_u->dun_d += 1;
  c3_assert(god_u->dun_d == wit_u->evt_d);

  god_u->mug_l = mug_l;

  c3_assert(wit_u->act == 0);
  wit_u->act = act;
}

/* _pier_work_replace(): worker reported replacement.
*/
static void
_pier_work_replace(u3_writ* wit_u,
                   u3_noun  job,
                   u3_noun  mat)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_lord* god_u = pir_u->god_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): compute: replace\r\n", wit_u->evt_d);
#endif

  c3_assert(god_u->sen_d == wit_u->evt_d);

  /* move backward in work processing
  */
  {
    u3z(wit_u->job);
    wit_u->job = job;

    u3z(wit_u->mat);
    wit_u->mat = mat;

    god_u->sen_d -= 1;
  }
}

/* _pier_work_compute(): dispatch for processing.
*/
static void
_pier_work_compute(u3_writ* wit_u)
{
  u3_pier* pir_u = wit_u->pir_u;
  u3_lord* god_u = pir_u->god_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): compute: request\r\n", wit_u->evt_d);
#endif

  c3_assert(wit_u->evt_d == (1 + god_u->sen_d));

  wit_u->mug_l = god_u->mug_l;

  _pier_work_build(wit_u);
  _pier_work_send(wit_u);

  god_u->sen_d += 1;
}

/* _pier_apply(): react to i/o, inbound or outbound.
*/
static void
_pier_apply(u3_pier* pir_u)
{
  u3_disk* log_u = pir_u->log_u;
  u3_lord* god_u = pir_u->god_u;
  u3_save* sav_u = pir_u->sav_u;

  if ( (0 == log_u) ||
       (0 == god_u) ||
       (c3n == god_u->liv_o) ||
       (u3_psat_init == pir_u->sat_e) )
  {
    return;
  }

  u3_writ* wit_u;
  c3_o     act_o = c3n;

start:

  /* iterate from queue exit, advancing any writs that can advance
  */
  wit_u = pir_u->ext_u;
  while ( wit_u ) {
    /* if writ is (a) next in line to compute, (b) worker is inactive,
    ** and (c) a snapshot has not been requested, request computation
    */
    if ( (wit_u->evt_d == (1 + god_u->sen_d)) &&
         (god_u->sen_d == god_u->dun_d) &&
         (sav_u->dun_d == sav_u->req_d) )
    {
      _pier_work_compute(wit_u);
      act_o = c3y;
    }

    /* if writ is (a) computed and (b) next in line to commit,
    ** and (c) no commit is in progress and (d) we've booted,
    ** request commit.
    */
    if ( (wit_u->evt_d <= god_u->dun_d) &&
         (wit_u->evt_d == (1 + log_u->moc_d)) &&
         (wit_u->evt_d == (1 + log_u->com_d)) )
    {
      _pier_disk_commit_request(wit_u);
      act_o = c3y;
    }

    /* if writ is (a) committed and (b) computed,
    ** release effects and delete from queue
    */
    if ( (wit_u->evt_d <= log_u->com_d) &&
         (wit_u->evt_d <= god_u->dun_d) )
    {
      //  effects must be released in order
      //
      c3_assert(wit_u == pir_u->ext_u);

      //  remove from queue
      //
      //    XX must be done before releasing effects
      //    which is currently reentrant
      //
      _pier_writ_unlink(wit_u);

      //  release effects
      //
      _pier_work_release(wit_u);

      //  free writ
      //
      _pier_writ_dispose(wit_u);

      wit_u = pir_u->ext_u;
      act_o = c3y;
    }
    else {
      /* otherwise, continue backward
      */
      wit_u = wit_u->nex_u;
    }
  }

  /* if we did anything to the queue, make another pass.
  */
  if ( c3y == act_o ) {
    act_o = c3n;
    goto start;
  }
}

/* _pier_set_ship():
*/
static void
_pier_set_ship(u3_pier* pir_u, u3_noun who, u3_noun fak)
{
  c3_assert( c3y == u3ud(who) );
  c3_assert( (c3y == fak) || (c3n == fak) );

  c3_o fak_o = fak;
  c3_d who_d[2];

  u3r_chubs(0, 2, who_d, who);

  c3_assert( ( (0 == pir_u->fak_o) &&
               (0 == pir_u->who_d[0]) &&
               (0 == pir_u->who_d[1]) ) ||
             ( (fak_o == pir_u->fak_o) &&
               (who_d[0] == pir_u->who_d[0]) &&
               (who_d[1] == pir_u->who_d[1]) ) );

  pir_u->fak_o = fak_o;
  pir_u->who_d[0] = who_d[0];
  pir_u->who_d[1] = who_d[1];

  {
    u3_noun how = u3dc("scot", 'p', u3k(who));

    c3_free(pir_u->who_c);
    pir_u->who_c = u3r_string(how);
    u3z(how);
  }

  //  Disable networking for fake ships
  //
  if ( c3y == pir_u->fak_o ) {
    u3_Host.ops_u.net = c3n;
  }

  u3z(who); u3z(fak);
}

/* _pier_disk_read_header_complete():
** XX async
*/
static void
_pier_disk_read_header_complete(u3_disk* log_u, u3_noun dat)
{
  u3_pier* pir_u = log_u->pir_u;

  {
    u3_noun who, fak, len;

    u3x_trel(dat, &who, &fak, &len);

    c3_assert( c3y == u3ud(who) );
    c3_assert( 1 >= u3r_met(7, who) );
    c3_assert( c3y == u3ud(fak) );
    c3_assert( 1 >= u3r_met(0, fak) );
    c3_assert( c3y == u3ud(len) );
    c3_assert( 1 >= u3r_met(3, len) );

    _pier_set_ship(pir_u, u3k(who), u3k(fak));

    pir_u->lif_d = u3r_chub(0, len);
  }

  u3z(dat);
}

/* _pier_disk_read_header():
*/
static void
_pier_disk_read_header(u3_disk* log_u)
{
  //  XX disabled
  //
  //    This is very, very slow.
  //    The one situation in which we currently *need* it -
  //    full log replay - it's unnecessary thanks to the current
  //    _pier_disk_load_commit.
  //    In all other situations, we're covered by
  //    _pier_work_play or u3_pier_boot.
  //    In the long run, it seems best to always get identity
  //    from the log for restart/replay.
  //
#if 0
  c3_assert( 0 != log_u->fol_u );

  c3_d pos_d = log_u->fol_u->end_d;
  c3_o got_o = c3n;

  c3_assert( 0ULL != pos_d );

  while ( pos_d ) {
    c3_d  len_d, evt_d;
    c3_d* buf_d;
    u3_noun mat, ovo, job, evt;

    buf_d = u3_foil_reveal(log_u->fol_u, &pos_d, &len_d);

    if ( !buf_d ) {
      _pier_disk_bail(0, "corrupt header");
      return;
    }

    if ( 0ULL == pos_d) {
      u3_noun mat = u3i_chubs(len_d, buf_d);
      u3_noun ovo = u3ke_cue(u3k(mat));

      c3_assert( c3__boot == u3h(ovo) );

      _pier_disk_read_header_complete(log_u, u3k(u3t(ovo)));

      u3z(ovo); u3z(mat);
    }

    c3_free(buf_d);
  }
#endif
}

/* _pier_disk_load_commit(): load all commits >= evt_d; set ent_u, ext_u.
** XX async
*/
static c3_o
_pier_disk_load_commit(u3_pier* pir_u,
                       c3_d     lav_d)

{
  u3_disk* log_u = pir_u->log_u;

  if ( !log_u->fol_u ) {
    return c3n;
  }
  else {
    c3_d pos_d = log_u->fol_u->end_d;
    c3_d old_d = 0;

#ifdef VERBOSE_EVENTS
    fprintf(stderr, "pier: load: commit: at %" PRIx64 "\r\n", pos_d);
#endif

    while ( pos_d ) {
      c3_d  len_d, evt_d;
      c3_d* buf_d;
      u3_noun mat, ovo, job, evt;

      buf_d = u3_foil_reveal(log_u->fol_u, &pos_d, &len_d);
      if ( !buf_d ) {
        _pier_disk_bail(0, "load: commit: corrupt");
        return c3n;
      }

      mat = u3i_chubs(len_d, buf_d);
      c3_free(buf_d);

      ovo = u3ke_cue(u3k(mat));

      //  reached header
      //
      if ( 0ULL == pos_d ) {
        c3_assert( 1ULL == lav_d );
        c3_assert( c3__boot == u3h(ovo) );

        _pier_disk_read_header_complete(log_u, u3k(u3t(ovo)));

        u3z(ovo); u3z(mat);
        break;
      }

      c3_assert(c3__work == u3h(ovo));
      evt = u3h(u3t(ovo));
      job = u3k(u3t(u3t(u3t(ovo))));
      evt_d = u3r_chub(0, evt);
      u3z(ovo);

      //  confirm event order
      //
      if ( (0 != old_d) &&
           ((old_d - 1ULL) != evt_d) ) {
        fprintf(stderr, "pier: load: event order\r\n");
        return c3n;
      }
      else {
        old_d = evt_d;
      }

      if ( evt_d < lav_d ) {
        u3z(mat);
        u3z(job);

        return c3y;
      }
      else {
        u3_writ* wit_u = c3_calloc(sizeof(u3_writ));

#ifdef VERBOSE_EVENTS
        fprintf(stderr, "pier: load: commit: %" PRIu64 "\r\n", evt_d);
#endif

        wit_u->pir_u = pir_u;
        wit_u->evt_d = evt_d;
        wit_u->job = job;
        wit_u->mat = mat;

        /* insert at queue exit -- the oldest events run first
        */
        if ( !pir_u->ent_u && !pir_u->ext_u ) {
          pir_u->ent_u = pir_u->ext_u = wit_u;
        }
        else {
          if ( (1ULL + wit_u->evt_d) != pir_u->ext_u->evt_d ) {
            fprintf(stderr, "pier: load: commit: event gap: %" PRIx64 ", %"
                             PRIx64 "\r\n",
                             wit_u->evt_d,
                             pir_u->ext_u->evt_d);
            u3z(mat);
            u3z(job);
            return c3n;
          }
          wit_u->nex_u = pir_u->ext_u;
          pir_u->ext_u = wit_u;
        }
      }
    }
    return c3y;
  }
}

/* _pier_boot_create(): create boot controller
*/
static u3_boot*
_pier_boot_create(u3_pier* pir_u, u3_noun pil, u3_noun ven)
{
  u3_boot* bot_u = c3_calloc(sizeof(u3_boot));
  bot_u->pil = u3k(pil);
  bot_u->ven = u3k(ven);
  bot_u->pir_u = pir_u;

  return bot_u;
}

/* _pier_boot_dispose(): dispose of boot controller
*/
static void
_pier_boot_dispose(u3_boot* bot_u)
{
  u3_pier* pir_u = bot_u->pir_u;

  u3z(bot_u->pil);
  u3z(bot_u->ven);
  free(bot_u);
  pir_u->bot_u = 0;
}

/* _pier_boot_vent(): create and enqueue boot sequence
**
**  per cgy:
**    this new boot sequence is almost, but not quite,
**    the right thing.  see new arvo.
*/
static void
_pier_boot_vent(u3_boot* bot_u)
{
  //  bot: boot formulas
  //  mod: module ova
  //  use: userpace ova
  //
  u3_noun bot, mod, use;
  u3_pier* pir_u = bot_u->pir_u;

  //  extract boot formulas and module/userspace ova from pill
  //
  {
    u3_noun pil_p, pil_q, pil_r;
    u3_noun pro;

    c3_assert( c3y == u3du(bot_u->pil) );

    if ( c3y == u3h(bot_u->pil) ) {
      u3x_trel(bot_u->pil, 0, &pil_p, &pil_q);
    }
    else {
      u3x_qual(bot_u->pil, 0, &pil_p, &pil_q, &pil_r);
    }

    pro = u3m_soft(0, u3ke_cue, u3k(pil_p));

    if ( 0 != u3h(pro) ) {
      fprintf(stderr, "boot: failed: unable to parse pill\r\n");
      exit(1);
    }

    u3x_trel(u3t(pro), &bot, &mod, &use);
    u3k(bot); u3k(mod); u3k(use);

    //  optionally replace filesystem in userspace
    //
    if ( c3y == u3h(bot_u->pil) ) {
      if ( u3_nul != pil_q ) {
        c3_w len_w = 0;
        u3_noun ova = use;
        u3_noun new = u3_nul;
        u3_noun ovo;

        while ( u3_nul != ova ) {
          ovo = u3h(ova);

          if ( c3__into == u3h(u3t(ovo)) ) {
            c3_assert( 0 == len_w );
            len_w++;
            ovo = u3k(u3t(pil_q));
          }

          new = u3nc(u3k(ovo), new);
          ova = u3t(ova);
        }

        c3_assert( 1 == len_w );

        u3z(use);
        use = u3kb_flop(new);
      }
    }
    //  prepend %lite module and userspace ova
    //
    else {
      mod = u3kb_weld(u3k(pil_q), mod);
      use = u3kb_weld(u3k(pil_r), use);
    }

    u3z(pro);
  }

  //  prepend entropy to the module sequence
  //
  //    XX also copy to _pier_loop_wake?
  //
  {
    c3_w    eny_w[16];
    c3_rand(eny_w);

    u3_noun wir = u3nt(u3_blip, c3__arvo, u3_nul);
    u3_noun car = u3nc(c3__wack, u3i_words(16, eny_w));

    mod = u3nc(u3nc(wir, car), mod);
  }

  //  prepend identity to the module sequence, setting single-home
  //
  {
    u3_noun wir = u3nt(u3_blip, c3__arvo, u3_nul);
    u3_noun car = u3nc(c3__whom, u3i_chubs(2, pir_u->who_d));

    mod = u3nc(u3nc(wir, car), mod);
  }

  //  insert boot sequence directly
  //
  //    Note that these are not ovum or (pair @da ovum) events,
  //    but raw nock formulas to be directly evaluated as the
  //    subject of the lifecycle formula [%2 [%0 3] %0 2].
  //    All subsequent events will be (pair @da ovum).
  //
  {
    u3_noun fol = bot;

    //  initialize the boot barrier
    //
    //    And the initial lifecycle boot barrier.
    //
    pir_u->but_d = u3kb_lent(u3k(fol));
    pir_u->lif_d = pir_u->but_d;

    while ( u3_nul != fol ) {
      _pier_insert(pir_u, 0, u3k(u3h(fol)));
      fol = u3t(fol);
    }
  }

  //  insert module events
  //
  {
    u3_noun ova = mod;
    //  add to the boot barrier
    //
    pir_u->but_d += u3kb_lent(u3k(ova));

    while ( u3_nul != ova ) {
      _pier_insert_ovum(pir_u, 0, u3k(u3h(ova)));
      ova = u3t(ova);
    }
  }

  //  XX moar boot sequence woes
  //
  //    some i/o must be initialized before legacy boot
  //
  {
    //  partially duplicates _pier_loop_wake()
    //
    c3_l cod_l;

    cod_l = u3a_lush(c3__ames);
    {
      //  stash domain for fake effect
      //
      //    XX this is horrible
      //
      u3_noun tuf = ( c3y == pir_u->fak_o ) ? u3_nul :
                    u3h(u3t(u3t(u3t(u3t(bot_u->ven)))));


      //  send a fake effect to bring up listeners and configure domains
      //
      //    XX horrible hack
      //
      u3_ames_ef_turf(pir_u, u3k(tuf));
    }

    u3_ames_ef_bake(pir_u);
    u3a_lop(cod_l);
  }

  //  insert legacy boot event
  //
  {
    //  XX do something about this wire
    //  XX route directly to %jael?
    //
    c3_assert( c3y == u3du(bot_u->ven) );

    u3_noun wir = u3nq(u3_blip, c3__term, '1', u3_nul);
    u3_noun car = u3nc(c3__boot, u3k(bot_u->ven));
    u3_noun ovo = u3nc(wir, car);

    _pier_insert_ovum(pir_u, 0, ovo);
  }

  //  insert userspace events
  //
  //    Currently just the initial filesystem
  //
  {
    u3_noun ova = use;

    while ( u3_nul != ova ) {
      _pier_insert_ovum(pir_u, 0, u3k(u3h(ova)));
      ova = u3t(ova);
    }
  }

  u3z(bot); u3z(mod); u3z(use);
}

/* _pier_boot_ready():
*/
static void
_pier_boot_ready(u3_pier* pir_u)
{
  u3_lord* god_u = pir_u->god_u;
  u3_disk* log_u = pir_u->log_u;

  c3_assert( u3_psat_init == pir_u->sat_e );

  if ( ( 0 == god_u) ||
       ( 0 == log_u) ||
       (c3y != god_u->liv_o) ||
       (c3y != log_u->liv_o) )
  {
    return;
  }

  //  mark all commits as released
  //
  god_u->rel_d = log_u->com_d;

  //  set next expected event number
  //
  pir_u->gen_d = (1ULL + log_u->com_d);

  //  boot
  //
  if ( 0 != pir_u->bot_u ) {
    c3_assert( 0 == log_u->com_d );
    c3_assert( 0 == god_u->dun_d );

    //  construct/enqueue boot sequence
    //
    _pier_boot_vent(pir_u->bot_u);
    _pier_boot_dispose(pir_u->bot_u);

    //  prepare serf for boot sequence, write log header
    //
    _pier_work_boot(pir_u, c3y);

    fprintf(stderr, "boot: ship: %s%s\r\n",
                     pir_u->who_c,
                     (c3y == pir_u->fak_o) ? " (fake)" : "");

    pir_u->sat_e = u3_psat_boot;
  }
  //  replay
  //
  else if ( god_u->dun_d < log_u->com_d ) {
    c3_assert( 0 != log_u->com_d );

    fprintf(stderr, "---------------- playback starting----------------\r\n");

    //  set the boot barrier to the last committed event
    //
    pir_u->but_d = log_u->com_d;

    //  begin queuing batches of committed events
    //
    //    XX batch, async
    //
    _pier_disk_load_commit(pir_u, (1ULL + god_u->dun_d));

    if ( 0 == god_u->dun_d ) {
      fprintf(stderr, "pier: replaying events 1 through %" PRIu64 "\r\n",
                      log_u->com_d);

      //  prepare serf for replay of boot sequence, don't write log header
      //
      _pier_work_boot(pir_u, c3n);
    }
    else {
      fprintf(stderr, "pier: replaying events %" PRIu64
                      " through %" PRIu64 "\r\n",
                      god_u->dun_d,
                      log_u->com_d);
    }

    pir_u->sat_e = u3_psat_pace;
  }
  //  resume
  //
  else {
    c3_assert( 0 != log_u->com_d );
    c3_assert( 0 != god_u->dun_d );

    //  set the boot barrier to the last computed event
    //
    pir_u->but_d = god_u->dun_d;

    //  resume normal operation
    //
    _pier_boot_complete(pir_u);
  }
}

/* _pier_disk_init_complete():
** XX async
*/
static void
_pier_disk_init_complete(u3_disk* log_u, c3_d evt_d)
{
  c3_assert( c3n == log_u->liv_o );
  log_u->liv_o = c3y;

  log_u->com_d = log_u->moc_d = evt_d;

  //  restore pier identity (XX currently a no-op, see comment)
  //
  _pier_disk_read_header(log_u);

  _pier_boot_ready(log_u->pir_u);
}

/* _pier_disk_init():
** XX async
*/
static c3_o
_pier_disk_init(u3_disk* log_u)
{
  c3_d evt_d = 0;
  c3_d pos_d = 0;

  c3_assert( c3n == log_u->liv_o );

  log_u->fol_u = u3_foil_absorb(log_u->com_u, "commit.urbit-log");

  if ( !log_u->fol_u ) {
    return c3n;
  }

  //  use the last event in the log to set the commit point.
  //
  if ( 0 != (pos_d = log_u->fol_u->end_d) ) {
    c3_d len_d = 0;

    c3_d* buf_d = u3_foil_reveal(log_u->fol_u, &pos_d, &len_d);

    if ( !buf_d ) {
      fprintf(stderr, "pier: load: commit: corrupt\r\n");
      return c3n;
    }

    {
      u3_noun mat = u3i_chubs(len_d, buf_d);
      u3_noun ovo = u3ke_cue(u3k(mat));

      c3_assert(c3__work == u3h(ovo));

      u3_noun evt = u3h(u3t(ovo));

      evt_d = u3r_chub(0, evt);

      u3z(mat); u3z(ovo); u3z(evt);
    }

#ifdef VERBOSE_EVENTS
    fprintf(stderr, "pier: load: last %" PRIu64 "\r\n", evt_d);
#endif

    c3_free(buf_d);
  }

  _pier_disk_init_complete(log_u, evt_d);

  return c3y;
}

/* _pier_disk_create(): load log for given point.
*/
static c3_o
_pier_disk_create(u3_pier* pir_u)
{
  u3_disk* log_u = c3_calloc(sizeof(*log_u));

  pir_u->log_u = log_u;
  log_u->pir_u = pir_u;
  log_u->liv_o = c3n;

  /* create/load pier, urbit directory, log directory.
  */
  {
    /* pier directory
    */
    {
      if ( 0 == (log_u->dir_u = u3_foil_folder(pir_u->pax_c)) ) {
        return c3n;
      }
    }

    /* pier/.urb
    */
    {
      c3_c* urb_c = c3_malloc(6 + strlen(pir_u->pax_c));

      strcpy(urb_c, pir_u->pax_c);
      strcat(urb_c, "/.urb");

      if ( 0 == (log_u->urb_u = u3_foil_folder(urb_c)) ) {
        c3_free(urb_c);
        return c3n;
      }
      c3_free(urb_c);
    }

    /* pier/.urb/log
    */
    {
      c3_c* log_c = c3_malloc(10 + strlen(pir_u->pax_c));

      strcpy(log_c, pir_u->pax_c);
      strcat(log_c, "/.urb/log");

      if ( 0 == (log_u->com_u = u3_foil_folder(log_c)) ) {
        c3_free(log_c);
        return c3n;
      }
      c3_free(log_c);
    }

    /* pier/.urb/put and pier/.urb/get
    */
    {
      c3_c* dir_c = c3_malloc(10 + strlen(pir_u->pax_c));

      strcpy(dir_c, pir_u->pax_c);
      strcat(dir_c, "/.urb/put");
      mkdir(dir_c, 0700);

      strcpy(dir_c, pir_u->pax_c);
      strcat(dir_c, "/.urb/get");
      mkdir(dir_c, 0700);

      c3_free(dir_c);
    }
  }

  //  create/load event log
  //
  if ( c3n == _pier_disk_init(log_u) ) {
    return c3n;
  }

  return c3y;
}

/* _pier_work_play(): with active worker, create or load log.
*/
static void
_pier_work_play(u3_pier* pir_u,
                c3_d     lav_d,
                c3_l     mug_l)
{
  u3_lord* god_u = pir_u->god_u;

#ifdef VERBOSE_EVENTS
  fprintf(stderr, "pier: (%" PRIu64 "): boot at mug %x\r\n", lav_d, mug_l);
#endif

  c3_assert( c3n == god_u->liv_o );
  god_u->liv_o = c3y;

  //  all events in the serf are complete
  //
  god_u->rel_d = god_u->dun_d = god_u->sen_d = (lav_d - 1ULL);

  _pier_boot_ready(pir_u);
}

/* _pier_work_exit(): handle subprocess exit.
*/
static void
_pier_work_exit(uv_process_t* req_u,
                c3_ds         sas_i,
                c3_i          sig_i)
{
  u3_lord* god_u = (void *) req_u;
  u3_pier* pir_u = god_u->pir_u;

  fprintf(stderr, "pier: exit: status %" PRIu64 ", signal %d\r\n", sas_i, sig_i);
  uv_close((uv_handle_t*) req_u, 0);

  _pier_disk_shutdown(pir_u);
  _pier_work_shutdown(pir_u);
}

/* _pier_work_writ(): find writ by event number.
*/
static u3_writ*
_pier_work_writ(u3_pier* pir_u,
                c3_d     evt_d)
{
  u3_writ* wit_u;

  /* very unlikely to be O(n) and n is small
  */
  for ( wit_u = pir_u->ext_u; wit_u; wit_u = wit_u->nex_u ) {
    if ( evt_d == wit_u->evt_d ) {
      return wit_u;
    }
  }
  return 0;
}

/* _pier_work_poke(): handle subprocess result.  transfer nouns.
*/
static void
_pier_work_poke(void*   vod_p,
                u3_noun mat)
{
  u3_pier* pir_u = vod_p;
  u3_noun  jar   = u3ke_cue(u3k(mat));
  u3_noun  p_jar, q_jar, r_jar;

  if ( c3y != u3du(jar) ) {
    goto error;
  }

  switch ( u3h(jar) ) {
    default: goto error;

    //  the worker process starts with a %play task,
    //  which tells us where to start playback
    //  (and who we are, if it knows) XX remove in favor of event-log header
    //
    case c3__play: {
      c3_d lav_d;
      c3_l mug_l;

      if ( (c3n == u3r_qual(u3t(jar), 0, &p_jar, &q_jar, &r_jar)) ||
           (c3n == u3ud(p_jar)) ||
           (u3r_met(6, p_jar) != 1) ||
           (c3n == u3ud(q_jar)) ||
           (u3r_met(5, p_jar) != 1) ||
           (c3n == u3du(r_jar)) ||
           (c3n == u3ud(u3h(r_jar))) ||
           ((c3y != u3t(r_jar)) && (c3n != u3t(r_jar))) )
      {
        if ( u3_nul == u3t(jar) ) {
          lav_d = 1ULL;
          mug_l = 0;
        }
        else {
          goto error;
        }
      }

      if ( u3_nul != u3t(jar) ) {
        lav_d = u3r_chub(0, p_jar);
        mug_l = u3r_word(0, q_jar);

        //  single-home
        //
        _pier_set_ship(pir_u, u3k(u3h(r_jar)), u3k(u3t(r_jar)));
      }

      _pier_work_play(pir_u, lav_d, mug_l);
      break;
    }

    case c3__work: {
      if ( (c3n == u3r_qual(jar, 0, &p_jar, &q_jar, &r_jar)) ||
           (c3n == u3ud(p_jar)) ||
           (u3r_met(6, p_jar) != 1) ||
           (c3n == u3ud(q_jar)) ||
           (u3r_met(5, q_jar) > 1) )
      {
        goto error;
      }
      else {
        c3_d     evt_d = u3r_chub(0, p_jar);
        c3_l     mug_l = u3r_word(0, q_jar);
        u3_writ* wit_u = _pier_work_writ(pir_u, evt_d);

        if ( !wit_u || (mug_l && (mug_l != wit_u->mug_l)) ) {
          goto error;
        }
        {
          // XX not the right place to print an error!
          //
          u3m_p("wire", u3h(u3t(r_jar)));
          u3m_p("oust", u3h(u3t(u3t(wit_u->job))));
          u3m_p("with", u3h(u3t(u3t(r_jar))));
          if ( c3__crud == u3h(u3t(u3t(r_jar))) ) {
            u3_pier_punt(0, u3k(u3t(u3t(u3t(u3t(r_jar))))));
          }

        }
        fprintf(stderr, "pier: replace: %" PRIu64 "\r\n", evt_d);

        _pier_work_replace(wit_u, u3k(r_jar), u3k(mat));
      }
      break;
    }

    case c3__done: {
      if ( (c3n == u3r_qual(jar, 0, &p_jar, &q_jar, &r_jar)) ||
           (c3n == u3ud(p_jar)) ||
           (u3r_met(6, p_jar) != 1) ||
           (c3n == u3ud(q_jar)) ||
           (u3r_met(5, q_jar) > 1) )
      {
        goto error;
      }
      else {
        c3_d     evt_d = u3r_chub(0, p_jar);
        c3_l     mug_l = u3r_word(0, q_jar);
        u3_writ* wit_u = _pier_work_writ(pir_u, evt_d);

        if ( !wit_u ) {
          fprintf(stderr, "poke: no writ: %" PRIu64 "\r\n", evt_d);
          goto error;
        }
        _pier_work_complete(wit_u, mug_l, u3k(r_jar));
      }
      break;
    }
  }

  u3z(jar); u3z(mat);
  _pier_apply(pir_u);
  return;

  error: {
    u3z(jar); u3z(mat);
    _pier_work_bail(0, "bad jar");
  }
}

/* pier_work_create(): instantiate child process.
*/
u3_lord*
_pier_work_create(u3_pier* pir_u)
{
  u3_lord* god_u = c3_calloc(sizeof *god_u);

  pir_u->god_u = god_u;
  god_u->pir_u = pir_u;
  god_u->liv_o = c3n;

  /* spawn new process and connect to it
  */
  {
    c3_c* arg_c[5];
    c3_c* bin_c = u3_Host.wrk_c;
    c3_c* pax_c;
    c3_c  key_c[256];
    c3_c  wag_c[11];
    c3_i  err_i;

    pax_c = c3_malloc(1 + strlen(pir_u->pax_c));
    strcpy(pax_c, pir_u->pax_c);

    sprintf(key_c, "%" PRIx64 ":%" PRIx64 ":%" PRIx64 ":%" PRIx64 "",
                   pir_u->key_d[0],
                   pir_u->key_d[1],
                   pir_u->key_d[2],
                   pir_u->key_d[3]);

    sprintf(wag_c, "%u", pir_u->wag_w);

    arg_c[0] = bin_c;                   //  executable
    arg_c[1] = pax_c;                   //  path to checkpoint directory
    arg_c[2] = key_c;                   //  disk key
    arg_c[3] = wag_c;                   //  runtime config
    arg_c[4] = 0;

    uv_pipe_init(u3L, &god_u->inn_u.pyp_u, 0);
    uv_pipe_init(u3L, &god_u->out_u.pyp_u, 0);

    god_u->cod_u[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
    god_u->cod_u[0].data.stream = (uv_stream_t *)&god_u->inn_u;

    god_u->cod_u[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    god_u->cod_u[1].data.stream = (uv_stream_t *)&god_u->out_u;

    god_u->cod_u[2].flags = UV_INHERIT_FD;
    god_u->cod_u[2].data.fd = 2;

    god_u->ops_u.stdio = god_u->cod_u;
    god_u->ops_u.stdio_count = 3;

    god_u->ops_u.exit_cb = _pier_work_exit;
    god_u->ops_u.file = arg_c[0];
    god_u->ops_u.args = arg_c;

    fprintf(stderr, "pier: spawn\r\n");
    if ( (err_i = uv_spawn(u3L, &god_u->cub_u, &god_u->ops_u)) ) {
      fprintf(stderr, "spawn: %s: %s\r\n", arg_c[0], uv_strerror(err_i));

      return 0;
    }
  }

  /* start reading from proc
  */
  {
    god_u->out_u.vod_p = pir_u;
    god_u->out_u.pok_f = _pier_work_poke;
    god_u->out_u.bal_f = _pier_work_bail;

    god_u->inn_u.bal_f = _pier_work_bail;

    u3_newt_read(&god_u->out_u);
  }
  return god_u;
}

/* u3_pier_create(): create a pier, loading existing.
*/
u3_pier*
u3_pier_create(c3_w wag_w, c3_c* pax_c)
{
  //  create pier
  //
  u3_pier* pir_u = c3_calloc(sizeof *pir_u);

  pir_u->pax_c = pax_c;
  pir_u->wag_w = wag_w;
  pir_u->sat_e = u3_psat_init;

  pir_u->sam_u = c3_calloc(sizeof(u3_ames));
  pir_u->teh_u = c3_calloc(sizeof(u3_behn));
  pir_u->unx_u = c3_calloc(sizeof(u3_unix));
  pir_u->sav_u = c3_calloc(sizeof(u3_save));

  //  initialize persistence
  //
  if ( c3n == _pier_disk_create(pir_u) ) {
    return 0;
  }

  //  start the serf process
  //
  if ( !(pir_u->god_u = _pier_work_create(pir_u)) ) {
    return 0;
  }

  //  install in the pier table
  //
  if ( 0 == u3K.all_w ) {
    u3K.all_w = 16;
    u3K.tab_u = c3_malloc(16 * sizeof(u3_pier*));
  }
  if ( u3K.len_w == u3K.all_w ) {
    u3K.all_w = 2 * u3K.all_w;
    u3K.tab_u = c3_realloc(u3K.tab_u, u3K.all_w * sizeof(u3_pier*));
  }
  u3K.tab_u[u3K.len_w++] = pir_u;

  return pir_u;
}

/* u3_pier_interrupt(): interrupt running process.
*/
void
u3_pier_interrupt(u3_pier* pir_u)
{
  uv_process_kill(&pir_u->god_u->cub_u, SIGINT);
}

/* u3_pier_discover(): insert task into process controller.
*/
void
u3_pier_discover(u3_pier* pir_u,
                 c3_l     msc_l,
                 u3_noun  job)
{
  _pier_insert(pir_u, msc_l, job);
  _pier_apply(pir_u);
}

/* _pier_exit_done(): synchronously shutting down
*/
static void
_pier_exit_done(u3_pier* pir_u)
{
  fprintf(stderr, "pier: exit\r\n");

  _pier_work_shutdown(pir_u);
  _pier_loop_exit(pir_u);

  //  XX uninstall pier from u3K.tab_u, dispose

  //  XX no can do
  //
  uv_stop(u3L);
}

/* u3_pier_exit(): trigger a gentle shutdown.
*/
void
u3_pier_exit(u3_pier* pir_u)
{
  pir_u->sat_e = u3_psat_done;

  //  XX must wait for callback confirming
  //
  u3_pier_snap(pir_u);
}

/* u3_pier_send(): modern send with target and path.
*/
void
u3_pier_send(u3_pier* pir_u, u3_noun pax, u3_noun tag, u3_noun fav)
{
}

/* u3_pier_work(): send event; real pier pointer.
**
**    XX: u3_pier_work() is for legacy events sent to a real pier.
*/
void
u3_pier_work(u3_pier* pir_u, u3_noun pax, u3_noun fav)
{
  u3_noun        now;
  struct timeval tim_tv;

  gettimeofday(&tim_tv, 0);
  now = u3_time_in_tv(&tim_tv);

  u3_pier_discover(pir_u, 0, u3nt(now, pax, fav));
}

/* u3_pier_plan(): send event; fake pier pointer
**
**    XX: u3_pier_plan() is maximum legacy, do not use.
*/
void
u3_pier_plan(u3_noun pax, u3_noun fav)
{
  u3_pier_work(u3_pier_stub(), pax, fav);
}

/* c3_rand(): fill a 512-bit (16-word) buffer.
*/
void
c3_rand(c3_w* rad_w)
{
  if ( 0 != ent_getentropy(rad_w, 64) ) {
    uL(fprintf(uH, "c3_rand getentropy: %s\n", strerror(errno)));
    //  XX review
    //
    u3_pier_bail();
  }
}

/* _pier_loop_time(): set time.
*/
static void
_pier_loop_time(void)
{
  struct timeval tim_tv;

  gettimeofday(&tim_tv, 0);
  u3v_time(u3_time_in_tv(&tim_tv));
}

/* _pier_loop_init_pier(): initialize loop handlers.
*/
static void
_pier_loop_init(u3_pier* pir_u)
{
  c3_l cod_l;

  _pier_loop_time();

  //  for i/o drivers that still use u3A->sen
  //
  u3v_numb();

  cod_l = u3a_lush(c3__ames);
  u3_ames_io_init(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__behn);
  u3_behn_io_init(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__unix);
  u3_unix_io_init(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__save);
  u3_save_io_init(pir_u);
  u3a_lop(cod_l);

  //  XX legacy handlers, not yet scoped to a pier
  //
  {
    cod_l = u3a_lush(c3__term);
    u3_term_io_init();
    u3a_lop(cod_l);

    cod_l = u3a_lush(c3__http);
    u3_http_io_init();
    u3a_lop(cod_l);

    cod_l = u3a_lush(c3__cttp);
    u3_cttp_io_init();
    u3a_lop(cod_l);
  }
}

/* _pier_loop_wake(): initialize listeners and send initial events.
*/
static void
_pier_loop_wake(u3_pier* pir_u)
{
  c3_l cod_l;

  cod_l = u3a_lush(c3__unix);
  u3_unix_io_talk(pir_u);
  u3_unix_ef_bake(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__ames);
  u3_ames_io_talk(pir_u);
  u3_ames_ef_bake(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__behn);
  u3_behn_ef_bake(pir_u);
  u3a_lop(cod_l);

  //  XX legacy handlers, not yet scoped to a pier
  //
  {
    cod_l = u3a_lush(c3__http);
    u3_http_io_talk();
    u3_http_ef_bake();
    u3a_lop(cod_l);

    cod_l = u3a_lush(c3__term);
    u3_term_io_talk();
    u3_term_ef_bake();
    u3a_lop(cod_l);
  }
}

/* _pier_loop_exit(): terminate I/O across the process.
*/
static void
_pier_loop_exit(u3_pier* pir_u)
{
  c3_l cod_l;

  cod_l = u3a_lush(c3__unix);
  u3_unix_io_exit(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__ames);
  u3_ames_io_exit(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__save);
  u3_save_io_exit(pir_u);
  u3a_lop(cod_l);

  cod_l = u3a_lush(c3__behn);
  u3_behn_io_exit(pir_u);
  u3a_lop(cod_l);

  //  XX legacy handlers, not yet scoped to a pier
  //
  {
    cod_l = u3a_lush(c3__term);
    u3_term_io_exit();
    u3a_lop(cod_l);

    cod_l = u3a_lush(c3__http);
    u3_http_io_exit();
    u3a_lop(cod_l);

    cod_l = u3a_lush(c3__cttp);
    u3_cttp_io_exit();
    u3a_lop(cod_l);
  }
}

/* _pier_boot_complete(): start organic event flow on boot/reboot.
*/
static void
_pier_boot_complete(u3_pier* pir_u)
{
  if ( u3_psat_init != pir_u->sat_e ) {
    u3_pier_snap(pir_u);
  }

  if ( u3_psat_boot == pir_u->sat_e ) {
    fprintf(stderr, "pier: boot complete\r\n");
  }
  else if ( u3_psat_pace == pir_u->sat_e ) {
    fprintf(stderr, "\n\r---------------- playback complete----------------\r\n");
  }

  pir_u->sat_e = u3_psat_play;

  //  the main course
  //
  _pier_loop_wake(pir_u);

  //  XX where should this go?
  //
  {
    if ( c3y == u3_Host.ops_u.veb ) {
      u3_term_ef_verb();
    }
  }
}

/* _pier_loop_prepare():
*/
static void
_pier_loop_prepare(uv_prepare_t* pep_u)
{
  _pier_loop_time();
}

/* u3_pier_bail(): clean up all event state.
*/
void
u3_pier_bail(void)
{
  fflush(stdout);
  u3_pier_exit(u3_pier_stub());

  exit(1);
}

/* _pier_tape(): dump a tape, old style.  Don't do this.
*/
static void
_pier_tape(FILE* fil_u, u3_noun tep)
{
  u3_noun tap = tep;

  while ( c3y == u3du(tap) ) {
    c3_c car_c;

    if ( u3h(tap) >= 127 ) {
      car_c = '?';
    } else car_c = u3h(tap);

    putc(car_c, fil_u);
    tap = u3t(tap);
  }
  u3z(tep);
}

/* _pier_wall(): dump a wall, old style.  Don't do this.
*/
static void
_pier_wall(u3_noun wol)
{
  FILE* fil_u = u3_term_io_hija();
  u3_noun wal = wol;

  fil_u = stderr;  // XX
  while ( u3_nul != wal ) {
    _pier_tape(fil_u, u3k(u3h(wal)));

    putc(13, fil_u);
    putc(10, fil_u);

    wal = u3t(wal);
  }
  u3_term_io_loja(0);
  u3z(wol);
}

/* u3_pier_tank(): dump single tank.
*/
void
u3_pier_tank(c3_l tab_l, u3_noun tac)
{
  u3_pier_punt(tab_l, u3nc(tac, u3_nul));
}

/* u3_pier_punt(): dump tank list.
*/
void
u3_pier_punt(c3_l tab_l, u3_noun tac)
{
  u3_noun blu   = u3_term_get_blew(0);
  c3_l    col_l = u3h(blu);
  u3_noun cat   = tac;

  //  We are calling nock here, but hopefully need no protection.
  //
  while ( c3y == u3r_du(cat) ) {
    if ( 0 == u3A->roc ) {
      u3_noun act = u3h(cat);

      if ( c3__leaf == u3h(act) ) {
        FILE* fil_u = u3_term_io_hija();

        fil_u = stderr;   // XX
        _pier_tape(fil_u, u3k(u3t(act)));
        putc(13, fil_u);
        putc(10, fil_u);

        u3_term_io_loja(0);
      }
    }
    else {
      u3_noun wol = u3dc("wash", u3nc(tab_l, col_l), u3k(u3h(cat)));

      _pier_wall(wol);
    }
    cat = u3t(cat);
  }
  u3z(tac);
  u3z(blu);
}

/* u3_pier_sway(): print trace.
*/
void
u3_pier_sway(c3_l tab_l, u3_noun tax)
{
  u3_noun mok = u3dc("mook", 2, tax);

  u3_pier_punt(tab_l, u3k(u3t(mok)));
  u3z(mok);
}

/* u3_pier_stub(): get the One Pier for unreconstructed code.
*/
u3_pier*
u3_pier_stub(void)
{
  if ( 0 == u3K.len_w ) {
    c3_assert(!"plan: no pier");
  }
  else {
    return u3K.tab_u[0];
  }
}

/* u3_pier_boot(): start the new pier system.
*/
void
u3_pier_boot(c3_w  wag_w,                   //  config flags
             u3_noun who,                   //  identity
             u3_noun ven,                   //  boot event
             u3_noun pil,                   //  type-of/path-to pill
             u3_noun pax)                   //  path to pier
{
  //  make/load pier
  //
  u3_pier* pir_u = u3_pier_create(wag_w, u3r_string(pax));

  //  set boot params
  //
  {
    pir_u->bot_u = _pier_boot_create(pir_u, u3k(pil), u3k(ven));

    _pier_set_ship(pir_u, u3k(who), ( c3__fake == u3h(ven) ) ? c3y : c3n);
  }

  //  initialize i/o handlers
  //
  _pier_loop_init(pir_u);

  //  initialize polling handle
  //
  uv_prepare_init(u3_Host.lup_u, &pir_u->pep_u);
  uv_prepare_start(&pir_u->pep_u, _pier_loop_prepare);

  u3z(who); u3z(ven); u3z(pil); u3z(pax);
}

/* u3_pier_stay(): resume the new pier system.
*/
void
u3_pier_stay(c3_w wag_w, u3_noun pax)
{
  //  make/load pier
  //
  u3_pier* pir_u = u3_pier_create(wag_w, u3r_string(pax));

  //  initialize i/o handlers
  //
  _pier_loop_init(pir_u);

  //  initialize polling handle
  //
  uv_prepare_init(u3_Host.lup_u, &pir_u->pep_u);
  uv_prepare_start(&pir_u->pep_u, _pier_loop_prepare);

  u3z(pax);
}

/* u3_pier_mark(): mark all Loom allocations in all u3_pier structs.
*/
c3_w
u3_pier_mark(FILE* fil_u)
{
  c3_w len_w = u3K.len_w;
  c3_w tot_w = 0;
  u3_pier* pir_u;

  while ( 0 < len_w ) {
    pir_u = u3K.tab_u[--len_w];
    fprintf(stderr, "pier: %u\r\n", len_w);

    if ( 0 != pir_u->bot_u ) {
      tot_w += u3a_maid(fil_u, "  boot event", u3a_mark_noun(pir_u->bot_u->ven));
      tot_w += u3a_maid(fil_u, "  pill", u3a_mark_noun(pir_u->bot_u->pil));
    }

    {
      u3_writ* wit_u = pir_u->ent_u;
      c3_w wit_w = 0;

      while ( 0 != wit_u ) {
        wit_w += u3a_mark_noun(wit_u->job);
        wit_w += u3a_mark_noun(wit_u->now);
        wit_w += u3a_mark_noun(wit_u->mat);
        wit_w += u3a_mark_noun(wit_u->act);
        wit_u = wit_u->nex_u;
      }

      tot_w += u3a_maid(fil_u, "  writs", wit_w);
    }
  }

  return tot_w;
}
