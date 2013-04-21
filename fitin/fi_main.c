
/*--------------------------------------------------------------------*/
/*--- FITIn: The fault inejction tool                    fi_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of FITIn, a small fault injection tool.

   Copyright (C) 2012 Clemens Terasa clemens.terasa@tu-harburg.de

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
 */

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"
#include "pub_tool_clreq.h"

#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_replacemalloc.h"
#include "pub_tool_xarray.h"
#include "pub_tool_aspacemgr.h"
#include "libvex_guest_amd64.h"

#include "fitin.h"
#include "fi_reg.h"

#if __x86_64__
#define SIZE_SUFFIX(n) n ## 64
#else
#define SIZE_SUFFIX(n) n ## 32
#endif

static const unsigned int MAX_STR_SIZE = 512;
static enum exitValues {
    EXIT_SUCCESS,
    EXIT_FAIL,
    EXIT_STOPPED,
};

static void fi_fini(Int exitcode);

/* Monitorables are basically memory locations of which the load operations are counted.
   These ma be extendend du to the needs of the tool.
*/
typedef struct _Monitorable {
    // Monitorable memory address; TODO: Find out how to reset it. (after function return?)
    Addr monAddr;
    // The size of one monitorable element
    UWord monSize;
    // The number of monitorable elements (i.e. array elements)
    UWord monLen;
    // # of loads
    ULong monLoads;
    // # of stores
    ULong monWrites;
    // valid address (may be invalidated due to function return)
    Bool monValid;
    // Bit to pos modified (only valid if modIteration != 0)
    UChar modBit;
    // # of loads until modification. (0 means never, 1 means on first load and so on)
    ULong modIteration;
    // Last write instruction
    ULong monLstWriteIsn;
} Monitorable;

/* Compare two Monitorables. Needed for an ordered list */
static Int cmpMonitorable (void *v1, void *v2) {
    Monitorable m1 = *(Monitorable *)v1;
    Monitorable m2 = *(Monitorable *)v2;
    if (m1.monAddr < m2.monAddr) {
        return -1;
    }
    if (m1.monAddr > m2.monAddr) {
        return 1;
    }
    return 0;
}

static toolData tData;

static void initTData() {
    tData.instAddr = (Addr)NULL;
    tData.monitoredInst = False;
    tData.loads = 0;
    tData.filter = MT_FILTFUNC;
    tData.filtstr = "main";
    tData.instCnt = 0;
    tData.instLmt = 0;
    tData.modMemLoadTime = 1;
    tData.modBit = 0;
    tData.monLoadCnt = 0;
    tData.goldenRun = False;

    tData.monitorables = VG_(newXA)(VG_(malloc), "tData.init", VG_(free), sizeof(Monitorable));
    VG_(setCmpFnXA)(tData.monitorables, cmpMonitorable);
    VG_(sortXA)(tData.monitorables);

    tData.load_states = VG_(newXA)(VG_(malloc),
                                   "tData.loadStates.init",
                                   VG_(free),
                                   sizeof(LoadState));
}

/* Check whether the instruction at 'instAddr' is in the function with the name
   in 'fnc' */
static inline Bool instInFunc(Addr instAddr, Char *fnc) {
    Char fnname[MAX_STR_SIZE];
    return (VG_(get_fnname)(instAddr, fnname, sizeof(fnname))
            && !VG_(strcmp)(fnname, fnc));
}

/* If the stack shrinks the Monitorables should be inbalidated */
static __inline__
void fi_stop_using_mem_stack(const Addr a, const SizeT len) {
    Word size = 0, i = 0;
    XArray *mons = tData.monitorables;

    size = VG_(sizeXA)(mons);
    for (i = 0; i < size; i++) {
        Monitorable *mon = VG_(indexXA)(mons, i);
        tl_assert(mon != NULL);

        if(a <= mon->monAddr && mon->monAddr < (a + len)) {
            mon->monValid = False;
        }
    }
}
/* Checks whether an instruction with the address 'instAddr' comes from the include path 'incl'.
   We need debug information in the executable to do this.
   */
static inline Bool instInInclude(Addr instAddr, char *incl) {

    Char filename[MAX_STR_SIZE];
    Char dirname[MAX_STR_SIZE];
    Bool diravail;
    UInt linenum;
    Bool retval;

    if(!VG_(get_filename_linenum)(instAddr,
                                  filename, MAX_STR_SIZE,
                                  dirname, MAX_STR_SIZE, &diravail,
                                  &linenum)) {
        return False;
    }
    if(!diravail) {
        return False;
    }

    //check whether the dirname begins like the content of incl
    //TODO: Is there a better way than comparing strings?
    retval =  !VG_(strncmp)(dirname, incl, VG_(strlen)(incl));

    return retval;

}
/* Check whether the instruction is a possible valid target.
 */
static inline Bool monitorInst(Addr instAddr) {
    if(VG_(get_fnname_kind_from_IP)(instAddr) == Vg_FnNameBelowMain) {
        return False;
    }
    switch (tData.filter) {
        case MT_FILTFUNC:
            return instInFunc(instAddr, tData.filtstr);
        case MT_FILTINCL:
            return instInInclude(instAddr, tData.filtstr);
        default:
            tl_assert(0);
            break;
    }
}

/* A simple instruction counter. */
static inline void incrInst() {
    tData.instCnt++;
    if(tData.instLmt && tData.instCnt >= tData.instLmt) {
        fi_fini(EXIT_STOPPED);
        VG_(exit)(0);
    }

}

/* FITIn uses several command line options.
   Valgrind helps to parse them.
 */
static Bool fi_process_cmd_line_option(Char *arg) {

    if VG_STR_CLO(arg, "--fnname", tData.filtstr) {
        tData.filter = MT_FILTFUNC;
    } else if VG_STR_CLO(arg, "--include", tData.filtstr) {
        tData.filter = MT_FILTINCL;
    } else if VG_INT_CLO(arg, "--mod-load-time", tData.modMemLoadTime) {}
    else if VG_INT_CLO(arg, "--mod-bit", tData.modBit) {}
    else if VG_INT_CLO(arg, "--inst-limit", tData.instLmt) {}
    else if VG_BOOL_CLO(arg, "--golden-run", tData.goldenRun) {}
    else if VG_BOOL_CLO(arg, "--persist-flip", tData.write_back_flip) {}
    else {
        return False;
    }

    tl_assert(tData.filtstr);
    tl_assert(tData.filtstr[0]);

    return True;
}

static void fi_print_usage(void) {
    VG_(printf)(
        "    --fnname=<name>           monitor instructions in functon <name> \n"
        "                              [main]\n"
        "    --include=<dir>           monitor instructions whci have debug   \n"
        "                              information from this directory        \n"
        "    --mod-load-time=<number>  modify at a given load time [0]        \n"
        "    --mod-bit=<number>        modify the given bit of the target     \n"
        "    --inst-limit=<number>     the maximum numbers of instructions to \n"
        "                              be executed. To prevent endless loops. \n"
        "    --golden-run=[yes|no]     States whether this is the golden run. \n"
        "                              The golden run just monitors, no modify\n"
        "    --persist-flip=[yes|no]   writes flipped data back to its memory \n"
        "                              origin\n"
    );
}

static void fi_print_debug_usage(void) {
    VG_(printf)(
        "    (none)\n"
    );
}

static void fi_post_clo_init(void) {
}

/**
 *  preLoadHelper
 *  This dirty helper function is inserted before every Load expression.
 *  It counts the overall Load expressions, the load operations of the monitorables,
 *  and might eventually modify the associated memory.
 *
 *  The returned value indicates whether the loaded address is revelant for variable
 *  tracing.
 */
static Word VEX_REGPARM(2) preLoadHelper(toolData *td, 
                                         Addr dataAddr) {
    LoadState state = (LoadState) { False, dataAddr };
    Monitorable key;
    Word first, last, state_list_size = VG_(sizeXA)(td->load_states);

    tl_assert(state_list_size != LOAD_STATE_INVALID_INDEX);

    // Always increment all overall load operations
    td->loads++;

    // FITIn-reg: no more injections to do
    if(td->injections != 0) {
        return 0;
    }

    key.monAddr = dataAddr;

    if(VG_(clo_verbosity) > 1) {
        VG_(printf)("Load: 0x%08x\n", dataAddr);
    }

    // iterate over monitorables list
    if(VG_(lookupXA)(td->monitorables, &key, &first, &last)) {
        Word i;
        Monitorable *mon = (Monitorable *)VG_(indexXA)(td->monitorables, first);

        if(!mon->monValid) {
            state.relevant = True;
        }
    }

    VG_(addToXA)(td->load_states, &state);

    return state_list_size;
}

/** 
 * Instrumentation of every LoadExpression.
 * The traversion of the statement exprssion tree probably not necessary in this detail
 *
 * The returned value is a pair of a load marker and an index to the load state list 
 * where relevancy and address can be found.
 */
static LoadData* instrument_load(toolData *td, IRExpr *expr, IRSB *sbOut) {
    if (expr->tag == Iex_Load) {
        IRDirty *di;
        IRExpr **args;
        IRStmt *st;
        LoadData *load_data = VG_(malloc)("fi.reg.load_data.intermediate", sizeof(LoadData));
  
        Int memSize = sizeofIRType(expr->Iex.Load.ty);
        args = mkIRExprVec_2(mkIRExpr_HWord(td),
                             expr->Iex.Load.addr);
        di = unsafeIRDirty_0_N(2,
                               "preLoadHelper",
                               VG_(fnptr_to_fnentry)(&preLoadHelper),
                               args);
        di->tmp = newIRTemp(sbOut->tyenv, SIZE_SUFFIX(Ity_I));

        st = IRStmt_Dirty(di);
        addStmtToIRSB(sbOut, st);

        if(VG_(clo_verbosity) > 1) {
            ppIRStmt(st);
            VG_(printf)("\nLoad instrumented: instAddr: 0x%08x\n", td->instAddr);
        }

        load_data->addr = expr->Iex.Load.addr;
        load_data->state_list_index = di->tmp;

        return load_data;
    }

    return NULL;
}

#define INSTRUMENT_ACCESS(expr) fi_reg_instrument_access(&tData, \
                                                         loads,\
                                                         replacements,\
                                                         (expr), \
                                                         sbOut)

static
IRSB *fi_instrument ( VgCallbackClosure *closure,
                      IRSB *sbIn,
                      VexGuestLayout *layout,
                      VexGuestExtents *vge,
                      IRType gWordTy, IRType hWordTy ) {
    IRSB      *sbOut;
    IRStmt    *st;
    IRExpr **argv;
    IRDirty *di;
    int i;
    XArray *loads = NULL, *replacements = NULL;

    /* We don't currently support this case. */
    if (gWordTy != hWordTy) {
        VG_(tool_panic)("host/guest word size mismatch");
    }

    // FITIn-reg: the list of loads
    loads = VG_(newXA)(VG_(malloc), 
                           "fi.reg.load.list",
                           VG_(free),
                           sizeof(LoadData)); 
    VG_(setCmpFnXA)(loads, fi_reg_compare_loads);
    VG_(sortXA)(loads);

    // FITIn-reg: the list of replacement rules for IRTemps
    replacements = VG_(newXA)(VG_(malloc),
                              "fi.reg.replace.list",
                              VG_(free),
                              sizeof(ReplaceData)); 
    VG_(setCmpFnXA)(replacements, fi_reg_compare_replacements);
    VG_(sortXA)(replacements);

    /* Set up SB */
    sbOut = deepCopyIRSBExceptStmts(sbIn);

    // Copy verbatim any IR preamble preceding the first IMark
    i = 0;
    while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
        addStmtToIRSB( sbOut, sbIn->stmts[i] );
        i++;
    }

    for (/*use current i*/; i < sbIn->stmts_used; i++) {
        st = sbIn->stmts[i];

        if (!st || st->tag == Ist_NoOp) {
            continue;
        }

        if(st->tag == Ist_IMark) {
            tData.instAddr = st->Ist.IMark.addr;
            tData.monitoredInst = monitorInst(tData.instAddr);

            argv = mkIRExprVec_0();
            di = unsafeIRDirty_0_N ( 0, "incrInst", VG_(fnptr_to_fnentry)(&incrInst), argv);
            addStmtToIRSB(sbOut, IRStmt_Dirty(di));
        }

        // Instrument IRExpressions. Look for loads in the hierarchy and instrument them.
        if(tData.monitoredInst) {
            switch (st->tag) {
                case Ist_AbiHint:
                case Ist_MBE:
                    break;
                case Ist_Put:
                    INSTRUMENT_ACCESS(st->Ist.Put.data);
                    fi_reg_set_occupancy(&tData, st->Ist.Put.offset, st->Ist.Put.data);
                    break;
                case Ist_PutI:
                    // FITIn-reg, needs further analysis
                    INSTRUMENT_ACCESS(st->Ist.PutI.details->ix);
                    INSTRUMENT_ACCESS(st->Ist.PutI.details->data);
                    break;
                case Ist_WrTmp: {
                    INSTRUMENT_ACCESS(st->Ist.WrTmp.data);
                    LoadData *load_data = instrument_load(&tData, st->Ist.WrTmp.data, sbOut);

                    if(load_data != NULL) {
                        load_data->dest_temp = st->Ist.WrTmp.tmp;
                        fi_reg_add_temp_load(loads, load_data);
                        VG_(free)(load_data);
                    } else {
                        fi_reg_add_load_on_get(loads, &tData, st->Ist.WrTmp.data);
                    }

                    fi_reg_instrument_access(&tData, loads, replacements, st->Ist.WrTmp.data, sbOut);
                    break;
                }
                case Ist_Store:
                    INSTRUMENT_ACCESS(st->Ist.Store.addr);
                    INSTRUMENT_ACCESS(st->Ist.Store.data);
                    break;
                case Ist_Dirty:
                    INSTRUMENT_ACCESS(st->Ist.Dirty.details->guard);
                    IRExpr **arg_ptr = st->Ist.Dirty.details->args;

                    while(*arg_ptr != NULL) {
                        INSTRUMENT_ACCESS(*arg_ptr);
                        arg_ptr++;
                    }

                    if(st->Ist.Dirty.details->mFx != Ifx_None) {
                        INSTRUMENT_ACCESS(st->Ist.Dirty.details->mAddr);
                    }
                    break;
                case Ist_CAS:                    
                    INSTRUMENT_ACCESS(st->Ist.CAS.details->addr);
                    INSTRUMENT_ACCESS(st->Ist.CAS.details->expdLo);
                    INSTRUMENT_ACCESS(st->Ist.CAS.details->expdHi);
                    INSTRUMENT_ACCESS(st->Ist.CAS.details->dataLo);
                    INSTRUMENT_ACCESS(st->Ist.CAS.details->dataHi);
                    break;
                case Ist_LLSC:                
                    INSTRUMENT_ACCESS(st->Ist.LLSC.addr);

                    if(st->Ist.LLSC.storedata != NULL) {
                        INSTRUMENT_ACCESS(st->Ist.LLSC.storedata);
                    }
                    break;
                case Ist_Exit:
                    INSTRUMENT_ACCESS(st->Ist.Exit.guard);
                    break;
                default:
                    break;
            }

            //print monitored instructions
            if(VG_(clo_verbosity) > 1) {
                ppIRStmt(st);
                VG_(printf)("\n");
            }
        }

        addStmtToIRSB(sbOut, st);
    }

    VG_(deleteXA)(replacements);
    VG_(deleteXA)(loads);

    return sbOut;
}

static void fi_fini(Int exitcode) {
    switch(exitcode) {
        case EXIT_FAIL:
            VG_(printf)("Exited with unknown cause!\n");
            break;
        case EXIT_STOPPED:
            VG_(printf)("Exited: Instruction limit reached!\n");
            break;
        case EXIT_SUCCESS:
        default:
            break;
    }

    XArray *mons = tData.monitorables;

    Word arrSize = VG_(sizeXA)(mons);
    Word i = 0;
    ULong monLoadCnt = 0;
    ULong monBytesLoad = 0;
    for (i = 0; i < arrSize; i++) {
        Monitorable *mon = (Monitorable *)VG_(indexXA)(mons, i);
        tl_assert(mon != NULL);
        //TODO: New repersentation of valid: valid up to modify

        if(VG_(clo_verbosity) > 1) {
            VG_(printf)("Monitorable memory address: 0x%016llX\n", mon->monAddr);
            VG_(printf)("                   size: %d\n", mon->monSize);
            VG_(printf)("                   loads: %d\n", mon->monLoads);
        }
        monLoadCnt += mon->monLoads;
        monBytesLoad += (mon->monSize * mon->monLoads);
    }

    VG_(deleteXA)(tData.monitorables);
    VG_(deleteXA)(tData.load_states);

    VG_(printf)("Totals:\n");
    VG_(printf)("Overall memory loads: %d\n", tData.loads);
    VG_(printf)("Monitored memory loads: %d\n", tData.monLoadCnt);
    VG_(printf)("Monitored memory bytes load: %d\n", monBytesLoad);
    VG_(printf)("Instructions executed: %d\n", tData.instCnt);
}

static
Bool fi_handle_client_request(ThreadId tid, UWord *args, UWord *ret) {
    if (!VG_IS_TOOL_USERREQ('F', 'I', args[0])
            && VG_USERREQ__GDB_MONITOR_COMMAND != args[0]) {
        return False;
    }

    switch(args[0]) {
        case VG_USERREQ__MON_VAR:
        case VG_USERREQ__MON_MEM: {

            //Initialize and add monitorable to list
            Monitorable mon;
            mon.monAddr = args[1];
            mon.monSize = args[2];
            mon.monLen  = 1; // Length is constant 1 because it's no array
            mon.monLoads = 0;
            mon.monWrites = 0;
            mon.monValid = True;
            mon.modBit = tData.modBit;
            mon.modIteration = tData.modMemLoadTime;
            mon.monLstWriteIsn = 0;

            Word first = 0, last = 0;

            if(VG_(lookupXA)(tData.monitorables, &mon, &first, &last)) {
                Word i;
                Monitorable *moni;
                for(i = first; i <= last; i++) {
                    moni = (Monitorable *)VG_(indexXA)(tData.monitorables, i);
                    tl_assert(moni != NULL);
                    if(moni->monValid) {
                        continue;
                    }
                }
                if(!moni->monValid) {
                    VG_(addToXA)(tData.monitorables, &mon);
                }
            } else {
                VG_(addToXA)(tData.monitorables, &mon);
            }

            VG_(sortXA)(tData.monitorables);

            break;
        }
        case VG_USERREQ__UMON_VAR:
        case VG_USERREQ__UMON_MEM: {
            Word size = 0, i = 0;
            XArray *mons = tData.monitorables;

            size = VG_(sizeXA)(mons);
            for (i = 0; i < size; i++) {
                Monitorable *mon = VG_(indexXA)(mons, i);
                tl_assert(mon != NULL);

                if( args[1] == mon->monAddr &&
                        args[2] == mon->monSize &&
                        mon->monValid == True) {
                    mon->monValid = False;
                }
            }
            break;
        }
        case VG_USERREQ__MON_ARR:
        case VG_USERREQ__UMON_ARR:
        case VG_USERREQ__INJ_B_VAR:
        case VG_USERREQ__INJ_B_MEM:
        case VG_USERREQ__CINJ_B_VAR:
        default:
            return False;
    }
    return True;
}

// The load states can be dropped after leaving client code to keep it
// small.
// ----------------------------------------------------------------------------
static void fi_reg_on_client_code_stop(ThreadId tid, ULong dispatched_blocks) {
    VG_(deleteXA)(tData.load_states);
    tData.load_states = VG_(newXA)(VG_(malloc),
                                   "fi.reg.loadStates.renew",
                                   VG_(free),
                                   sizeof(LoadState));
}

static void fi_pre_clo_init(void) {
    VG_(details_name)            ("FITIn");
    VG_(details_version)         (NULL);
    VG_(details_description)     ("A simple fault injection tool");
    VG_(details_copyright_author)(
        "Copyright (C) 2012, and GNU GPL'd, by Clemens Terasa.");
    VG_(details_bug_reports_to)  ("BLmadman@gmx.de");

    VG_(details_avg_translation_sizeB) ( 275 );

    VG_(basic_tool_funcs)        (fi_post_clo_init,
                                  fi_instrument,
                                  fi_fini);
    VG_(needs_command_line_options)(fi_process_cmd_line_option,
                                    fi_print_usage,
                                    fi_print_debug_usage);
    VG_(needs_client_requests)(fi_handle_client_request);
    initTData();
    VG_(track_die_mem_stack)(fi_stop_using_mem_stack);

    // FITIn-reg
    VG_(track_stop_client_code)(fi_reg_on_client_code_stop);
}

VG_DETERMINE_INTERFACE_VERSION(fi_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
