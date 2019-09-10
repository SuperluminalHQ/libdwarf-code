/*
  Copyright (C) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright (C) 2007-2019 David Anderson. All Rights Reserved.
  Portions Copyright 2012 SN Systems Ltd. All rights reserved.

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2.1 of the GNU Lesser General Public License
  as published by the Free Software Foundation.

  This program is distributed in the hope that it would be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

  Further, this software is distributed without any warranty that it is
  free of the rightful claim of any third person regarding infringement
  or the like.  Any license provided herein, whether implied or
  otherwise, applies only to this software file.  Patent licenses, if
  any, provided herein do not apply to combinations of this program with
  other software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General Public
  License along with this program; if not, write the Free Software
  Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston MA 02110-1301,
  USA.

*/

#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h> /* For free() and emergency abort() */
#endif /* HAVE_STDLIB_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#elif defined(_WIN32) && defined(_MSC_VER)
#include <io.h>
#endif /* HAVE_UNISTD_H */
#include <sys/types.h> /* for open() */
#include <sys/stat.h> /* for open() */
#include <fcntl.h> /* for open() */
#include "dwarf_incl.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_util.h"
#include "dwarf_abbrev.h"
#include "memcpy_swap.h"
#include "dwarf_die_deliv.h"
#include "pro_encode_nm.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif /* O_BINARY */



#define MINBUFLEN 1000
#define TRUE  1
#define FALSE 0

#if _WIN32
#define NULL_DEVICE_NAME "NUL"
#else
#define NULL_DEVICE_NAME "/dev/null"
#endif /* _WIN32 */

/*  The function returned allows dwarfdump and other callers to
    do an endian-sensitive copy-word with a chosen
    source-length.  */
typedef void (*endian_funcp_type)(void *, const void *,unsigned long);

#if 0
static void
dump_bytes(char * msg,Dwarf_Small * start, long len)
{
    Dwarf_Small *end = start + len;
    Dwarf_Small *cur = start;

    printf("%s ",msg);
    for (; cur < end; cur++) {
        printf("%02x ", *cur);
    }
    printf("\n");
}
#endif

endian_funcp_type
dwarf_get_endian_copy_function(Dwarf_Debug dbg)
{
    if (dbg) {
        return dbg->de_copy_word;
    }
    return 0;
}


Dwarf_Bool
_dwarf_file_has_debug_fission_cu_index(Dwarf_Debug dbg)
{
    if(!dbg) {
        return FALSE;
    }
    if (dbg->de_cu_hashindex_data) {
        return TRUE;
    }
    return FALSE;
}
Dwarf_Bool
_dwarf_file_has_debug_fission_tu_index(Dwarf_Debug dbg)
{
    if(!dbg) {
        return FALSE;
    }
    if (dbg->de_tu_hashindex_data ) {
        return TRUE;
    }
    return FALSE;
}


Dwarf_Bool
_dwarf_file_has_debug_fission_index(Dwarf_Debug dbg)
{
    if(!dbg) {
        return FALSE;
    }
    if (dbg->de_cu_hashindex_data ||
        dbg->de_tu_hashindex_data) {
        return 1;
    }
    return FALSE;
}

int
_dwarf_internal_get_die_comp_dir(Dwarf_Die die, const char **compdir_out,
    const char **compname_out,
    Dwarf_Error *error)
{
    Dwarf_Attribute comp_dir_attr = 0;
    Dwarf_Attribute comp_name_attr = 0;
    int resattr = 0;
    Dwarf_Debug dbg = 0;

    dbg = die->di_cu_context->cc_dbg;
    resattr = dwarf_attr(die, DW_AT_name, &comp_name_attr, error);
    if (resattr == DW_DLV_ERROR) {
        return resattr;
    }
    if (resattr == DW_DLV_OK) {
        int cres = DW_DLV_ERROR;
        char *name = 0;

        cres = dwarf_formstring(comp_name_attr, &name, error);
        if (cres == DW_DLV_ERROR) {
            dwarf_dealloc(dbg, comp_name_attr, DW_DLA_ATTR);
            return cres;
        } else if (cres == DW_DLV_OK) {
            *compname_out = (const char *)name;
        } else {
            /* FALL thru */
        }
    }
    if (resattr == DW_DLV_OK) {
        dwarf_dealloc(dbg, comp_name_attr, DW_DLA_ATTR);
    }
    resattr = dwarf_attr(die, DW_AT_comp_dir, &comp_dir_attr, error);
    if (resattr == DW_DLV_ERROR) {
        return resattr;
    }
    if (resattr == DW_DLV_OK) {
        int cres = DW_DLV_ERROR;
        char *cdir = 0;

        cres = dwarf_formstring(comp_dir_attr, &cdir, error);
        if (cres == DW_DLV_ERROR) {
            dwarf_dealloc(dbg, comp_dir_attr, DW_DLA_ATTR);
            return cres;
        } else if (cres == DW_DLV_OK) {
            *compdir_out = (const char *) cdir;
        } else {
            /* FALL thru */
        }
    }
    if (resattr == DW_DLV_OK) {
        dwarf_dealloc(dbg, comp_dir_attr, DW_DLA_ATTR);
    }
    return resattr;
}


/*  Given a form, and a pointer to the bytes encoding
    a value of that form, val_ptr, this function returns
    the length, in bytes, of a value of that form.
    When using this function, check for a return of 0
    a recursive DW_FORM_INDIRECT value.  */
int
_dwarf_get_size_of_val(Dwarf_Debug dbg,
    Dwarf_Unsigned form,
    Dwarf_Half cu_version,
    Dwarf_Half address_size,
    Dwarf_Small * val_ptr,
    int v_length_size,
    Dwarf_Unsigned *size_out,
    Dwarf_Small *section_end_ptr,
    Dwarf_Error*error)
{
    Dwarf_Unsigned length = 0;
    Dwarf_Unsigned leb128_length = 0;
    Dwarf_Unsigned form_indirect = 0;
    Dwarf_Unsigned ret_value = 0;

    switch (form) {

    /*  When we encounter a FORM here that
        we know about but forgot to enter here,
        we had better not just continue.
        Usually means we forgot to update this function
        when implementing form handling of a new FORM.
        Disaster results from using a bogus value,
        so generate error. */
    default:
        _dwarf_error(dbg,error,DW_DLE_DEBUG_FORM_HANDLING_INCOMPLETE);
        return DW_DLV_ERROR;


    case 0:  return DW_DLV_OK;

    case DW_FORM_GNU_ref_alt:
    case DW_FORM_GNU_strp_alt:
    case DW_FORM_strp_sup:
        *size_out = v_length_size;
        return DW_DLV_OK;

    case DW_FORM_addr:
        if (address_size) {
            *size_out = address_size;
        } else {
            /* This should never happen, address_size should be set. */
            *size_out = dbg->de_pointer_size;
        }
        return DW_DLV_OK;
    case DW_FORM_ref_sig8:
        *size_out = 8;
        /* sizeof Dwarf_Sig8 */
        return DW_DLV_OK;

    /*  DWARF2 was wrong on the size of the attribute for
        DW_FORM_ref_addr.  We assume compilers are using the
        corrected DWARF3 text (for 32bit pointer target objects pointer and
        offsets are the same size anyway).
        It is clear (as of 2014) that for 64bit folks used
        the V2 spec in the way V2 was
        written, so the ref_addr has to account for that.*/
    case DW_FORM_ref_addr:
        if (cu_version == DW_CU_VERSION2) {
            *size_out = address_size;
        } else {
            *size_out = v_length_size;
        }
        return DW_DLV_OK;

    case DW_FORM_block1: {
        ptrdiff_t sizeasptrdiff = 0;

        if (val_ptr >= section_end_ptr) {
            _dwarf_error(dbg,error,DW_DLE_FORM_BLOCK_LENGTH_ERROR);
            return DW_DLV_ERROR;
        }
        ret_value =  *(Dwarf_Small *) val_ptr;
        sizeasptrdiff = (ptrdiff_t)ret_value;
        if (sizeasptrdiff > (section_end_ptr - val_ptr) ||
            sizeasptrdiff < 0) {
            _dwarf_error(dbg,error,DW_DLE_FORM_BLOCK_LENGTH_ERROR);
            return DW_DLV_ERROR;
        }
        *size_out = ret_value +1;
        }
        return DW_DLV_OK;

    case DW_FORM_block2: {
        ptrdiff_t sizeasptrdiff = 0;

        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            val_ptr, DWARF_HALF_SIZE,error,section_end_ptr);
        sizeasptrdiff = (ptrdiff_t)ret_value;
        if (sizeasptrdiff > (section_end_ptr - val_ptr) ||
            sizeasptrdiff < 0) {
            _dwarf_error(dbg,error,DW_DLE_FORM_BLOCK_LENGTH_ERROR);
            return DW_DLV_ERROR;
        }
        *size_out = ret_value + DWARF_HALF_SIZE;
        }
        return DW_DLV_OK;

    case DW_FORM_block4: {
        ptrdiff_t sizeasptrdiff = 0;

        READ_UNALIGNED_CK(dbg, ret_value, Dwarf_Unsigned,
            val_ptr, DWARF_32BIT_SIZE,
            error,section_end_ptr);
        sizeasptrdiff = (ptrdiff_t)ret_value;
        if (sizeasptrdiff > (section_end_ptr - val_ptr) ||
            sizeasptrdiff < 0) {
            _dwarf_error(dbg,error,DW_DLE_FORM_BLOCK_LENGTH_ERROR);
            return DW_DLV_ERROR;
        }
        *size_out = ret_value + DWARF_32BIT_SIZE;
        }
        return DW_DLV_OK;

    case DW_FORM_data1:
        *size_out = 1;
        return DW_DLV_OK;

    case DW_FORM_data2:
        *size_out = 2;
        return DW_DLV_OK;

    case DW_FORM_data4:
        *size_out = 4;
        return DW_DLV_OK;

    case DW_FORM_data8:
        *size_out = 8;
        return DW_DLV_OK;

    case DW_FORM_data16:
        *size_out = 16;
        return DW_DLV_OK;

    case DW_FORM_string: {
        int res = 0;
        res = _dwarf_check_string_valid(dbg,val_ptr,
            val_ptr,
            section_end_ptr,
            DW_DLE_FORM_STRING_BAD_STRING,
            error);
        if ( res != DW_DLV_OK) {
            return res;
        }
        }
        *size_out = strlen((char *) val_ptr) + 1;
        return DW_DLV_OK;

    case DW_FORM_block:
    case DW_FORM_exprloc: {
        DECODE_LEB128_UWORD_LEN_CK(val_ptr,length,leb128_length,
            dbg,error,section_end_ptr);
        *size_out = length + leb128_length;
        return DW_DLV_OK;;
    }

    case DW_FORM_flag_present:
        *size_out = 0;
        return DW_DLV_OK;

    case DW_FORM_flag:
        *size_out = 1;
        return DW_DLV_OK;

    case DW_FORM_sec_offset:
        /* If 32bit dwarf, is 4. Else is 64bit dwarf and is 8. */
        *size_out = v_length_size;
        return DW_DLV_OK;

    case DW_FORM_ref_udata: {
        UNUSEDARG Dwarf_Unsigned v = 0;

        /*  Discard the decoded value, we just want the length
            of the value. */
        DECODE_LEB128_UWORD_LEN_CK(val_ptr,v,leb128_length,
            dbg,error,section_end_ptr);
        *size_out = leb128_length;
        return DW_DLV_OK;;
    }

    case DW_FORM_indirect:
        {
            Dwarf_Unsigned indir_len = 0;
            int res = 0;
            Dwarf_Unsigned info_data_len = 0;

            DECODE_LEB128_UWORD_LEN_CK(val_ptr,form_indirect,indir_len,
                dbg,error,section_end_ptr);
            if (form_indirect == DW_FORM_indirect) {
                /* We are in big trouble: The true form
                    of DW_FORM_indirect is
                    DW_FORM_indirect? Nonsense. Should
                    never happen. */
                _dwarf_error(dbg,error,DW_DLE_NESTED_FORM_INDIRECT_ERROR);
                return DW_DLV_ERROR;
            }
            /*  If form_indirect  is DW_FORM_implicit_const then
                the following call will set info_data_len 0 */
            res = _dwarf_get_size_of_val(dbg,
                form_indirect,
                cu_version,
                address_size,
                val_ptr + indir_len,
                v_length_size,
                &info_data_len,
                section_end_ptr,
                error);
            if(res != DW_DLV_OK) {
                return res;
            }
            *size_out = indir_len + info_data_len;
            return DW_DLV_OK;
        }

    case DW_FORM_ref1:
        *size_out = 1;
        return DW_DLV_OK;

    case DW_FORM_ref2:
        *size_out = 2;
        return DW_DLV_OK;

    case DW_FORM_ref4:
        *size_out = 4;
        return DW_DLV_OK;

    case DW_FORM_ref8:
        *size_out = 8;
        return DW_DLV_OK;

    /*  DW_FORM_implicit_const  is a value in the
        abbreviations, not in the DIEs and this
        functions measures DIE size. */
    case DW_FORM_implicit_const:
        *size_out = 0;
        return DW_DLV_OK;

    case DW_FORM_sdata: {
        /*  Discard the decoded value, we just want the length
            of the value. */
        UNUSEDARG Dwarf_Signed v = 0;

        /*  Discard the decoded value, we just want the length
            of the value. */
        DECODE_LEB128_SWORD_LEN_CK(val_ptr,v,leb128_length,
            dbg,error,section_end_ptr);
        *size_out = leb128_length;
        return DW_DLV_OK;
    }
    case DW_FORM_ref_sup4:
        *size_out = 4;
        return DW_DLV_OK;
    case DW_FORM_ref_sup8:
        *size_out = 8;
        return DW_DLV_OK;

    case DW_FORM_addrx1:
        *size_out = 1;
        return DW_DLV_OK;
    case DW_FORM_addrx2:
        *size_out = 2;
        return DW_DLV_OK;
    case DW_FORM_addrx3:
        *size_out = 4;
        return DW_DLV_OK;
    case DW_FORM_addrx4:
        *size_out = 4;
        return DW_DLV_OK;
    case DW_FORM_strx1:
        *size_out = 1;
        return DW_DLV_OK;
    case DW_FORM_strx2:
        *size_out = 2;
        return DW_DLV_OK;
    case DW_FORM_strx3:
        *size_out = 4;
        return DW_DLV_OK;
    case DW_FORM_strx4:
        *size_out = 4;
        return DW_DLV_OK;

    case DW_FORM_loclistx:
    case DW_FORM_rnglistx:
    case DW_FORM_addrx:
    case DW_FORM_GNU_addr_index:
    case DW_FORM_strx:
    case DW_FORM_GNU_str_index: {
        UNUSEDARG Dwarf_Unsigned v = 0;

        DECODE_LEB128_UWORD_LEN_CK(val_ptr,v,leb128_length,
            dbg,error,section_end_ptr);
        *size_out = leb128_length;
        return DW_DLV_OK;
    }

    case DW_FORM_line_strp:
    case DW_FORM_strp:
        *size_out = v_length_size;
        return DW_DLV_OK;

    case DW_FORM_udata: {
        /*  Discard the decoded value, we just want the length
            of the value. */
        UNUSEDARG Dwarf_Unsigned v = 0;

        DECODE_LEB128_UWORD_LEN_CK(val_ptr,v,leb128_length,
            dbg,error,section_end_ptr);
        *size_out = leb128_length;
        return DW_DLV_OK;
    }
    }
}

/*  We allow an arbitrary number of HT_MULTIPLE entries
    before resizing.  It seems up to 20 or 30
    would work nearly as well.
    We could have a different resize multiple than 'resize now'
    test multiple, but for now we don't do that.  */
#define HT_MULTIPLE 8

/*  Copy the old entries, updating each to be in
    a new list.  Don't delete anything. Leave the
    htin with stale data. */
static void
copy_abbrev_table_to_new_table(Dwarf_Hash_Table htin,
  Dwarf_Hash_Table htout)
{
    Dwarf_Hash_Table_Entry entry_in = htin->tb_entries;
    unsigned entry_in_count = htin->tb_table_entry_count;
    Dwarf_Hash_Table_Entry entry_out = htout->tb_entries;
    unsigned entry_out_count = htout->tb_table_entry_count;
    unsigned k = 0;
    for (; k < entry_in_count; ++k,++entry_in) {
        Dwarf_Abbrev_List listent = entry_in->at_head;
        Dwarf_Abbrev_List nextlistent = 0;

        for (; listent ; listent = nextlistent) {
            unsigned newtmp = listent->abl_code;
            unsigned newhash = newtmp%entry_out_count;
            Dwarf_Hash_Table_Entry e;
            nextlistent = listent->abl_next;
            e = entry_out+newhash;
            /*  Move_entry_to_new_hash. This reverses the
                order of the entries, effectively, but
                that does not seem significant. */
            listent->abl_next = e->at_head;
            e->at_head = listent;

            htout->tb_total_abbrev_count++;
        }
    }
}

/*  We allow zero form here, end of list. */
int
_dwarf_valid_form_we_know(Dwarf_Unsigned at_form,
    Dwarf_Unsigned at_name)
{
    if(at_form == 0 && at_name == 0) {
        return TRUE;
    }
    if (at_name == 0) {
        return FALSE;
    }
    if (at_form <= DW_FORM_addrx4 ) {
        return TRUE;
    }
    if (at_form == DW_FORM_GNU_addr_index ||
        at_form == DW_FORM_GNU_str_index  ||
        at_form == DW_FORM_GNU_ref_alt ||
        at_form == DW_FORM_GNU_strp_alt) {
        return TRUE;
    }
    return FALSE;
}

/*  This function returns a pointer to a Dwarf_Abbrev_List_s
    struct for the abbrev with the given code.  It puts the
    struct on the appropriate hash table.  It also adds all
    the abbrev between the last abbrev added and this one to
    the hash table.  In other words, the .debug_abbrev section
    is scanned sequentially from the top for an abbrev with
    the given code.  All intervening abbrevs are also put
    into the hash table.

    This function hashes the given code, and checks the chain
    at that hash table entry to see if a Dwarf_Abbrev_List_s
    with the given code exists.  If yes, it returns a pointer
    to that struct.  Otherwise, it scans the .debug_abbrev
    section from the last byte scanned for that CU till either
    an abbrev with the given code is found, or an abbrev code
    of 0 is read.  It puts Dwarf_Abbrev_List_s entries for all
    abbrev's read till that point into the hash table.  The
    hash table contains both a head pointer and a tail pointer
    for each entry.

    While the lists can move and entries can be moved between
    lists on reallocation, any given Dwarf_Abbrev_list entry
    never moves once allocated, so the pointer is safe to return.

    See also dwarf_get_abbrev() in dwarf_abbrev.c.

    Returns DW_DLV_ERROR on error.  */
int
_dwarf_get_abbrev_for_code(Dwarf_CU_Context cu_context,
    Dwarf_Unsigned code,
    Dwarf_Abbrev_List *list_out,
    Dwarf_Error *error)
{
    Dwarf_Debug dbg = cu_context->cc_dbg;
    Dwarf_Hash_Table hash_table_base = cu_context->cc_abbrev_hash_table;
    Dwarf_Hash_Table_Entry entry_base = 0;
    Dwarf_Hash_Table_Entry entry_cur = 0;
    Dwarf_Unsigned hash_num = 0;
    Dwarf_Unsigned abbrev_code = 0;
    Dwarf_Unsigned abbrev_tag  = 0;
    Dwarf_Abbrev_List hash_abbrev_entry = 0;
    Dwarf_Abbrev_List inner_list_entry = 0;
    Dwarf_Hash_Table_Entry inner_hash_entry = 0;

    Dwarf_Byte_Ptr abbrev_ptr = 0;
    Dwarf_Byte_Ptr end_abbrev_ptr = 0;
    unsigned hashable_val = 0;

    if (!hash_table_base->tb_entries) {
        hash_table_base->tb_table_entry_count =  HT_MULTIPLE;
        hash_table_base->tb_total_abbrev_count= 0;
        hash_table_base->tb_entries =
            (struct  Dwarf_Hash_Table_Entry_s *)_dwarf_get_alloc(dbg,
            DW_DLA_HASH_TABLE_ENTRY,
            hash_table_base->tb_table_entry_count);
        if (!hash_table_base->tb_entries) {
            return DW_DLV_NO_ENTRY;
        }

    } else if (hash_table_base->tb_total_abbrev_count >
        ( hash_table_base->tb_table_entry_count * HT_MULTIPLE) ) {
        struct Dwarf_Hash_Table_s newht;
        /* Effectively multiplies by >= HT_MULTIPLE */
        newht.tb_table_entry_count =  hash_table_base->tb_total_abbrev_count;
        newht.tb_total_abbrev_count = 0;
        newht.tb_entries =
            (struct  Dwarf_Hash_Table_Entry_s *)_dwarf_get_alloc(dbg,
            DW_DLA_HASH_TABLE_ENTRY,
            newht.tb_table_entry_count);

        if (!newht.tb_entries) {
            return DW_DLV_NO_ENTRY;
        }
        /*  Copy the existing entries to the new table,
            rehashing each.  */
        copy_abbrev_table_to_new_table(hash_table_base, &newht);
        /*  Dealloc only the entries hash table array, not the lists
            of things pointed to by a hash table entry array. */
        dwarf_dealloc(dbg, hash_table_base->tb_entries,DW_DLA_HASH_TABLE_ENTRY);
        hash_table_base->tb_entries = 0;
        /*  Now overwrite the existing table descriptor with
            the new, newly valid, contents. */
        *hash_table_base = newht;
    } /* Else is ok as is, add entry */

    hashable_val = code;
    hash_num = hashable_val %
        hash_table_base->tb_table_entry_count;
    entry_base = hash_table_base->tb_entries;
    entry_cur  = entry_base + hash_num;

    /* Determine if the 'code' is the list of synonyms already. */
    for (hash_abbrev_entry = entry_cur->at_head;
        hash_abbrev_entry != NULL && hash_abbrev_entry->abl_code != code;
        hash_abbrev_entry = hash_abbrev_entry->abl_next);
    if (hash_abbrev_entry != NULL) {
        /*  This returns a pointer to an abbrev list entry, not
            the list itself. */
        *list_out = hash_abbrev_entry;
        return DW_DLV_OK;
    }

    if (cu_context->cc_last_abbrev_ptr) {
        abbrev_ptr = cu_context->cc_last_abbrev_ptr;
        end_abbrev_ptr = cu_context->cc_last_abbrev_endptr;
    } else {
        /*  This is ok because cc_abbrev_offset includes DWP
            offset if appropriate. */
        abbrev_ptr = dbg->de_debug_abbrev.dss_data +
            cu_context->cc_abbrev_offset;

        if (cu_context->cc_dwp_offsets.pcu_type)  {
            /*  In a DWP the abbrevs
                for this context are known quite precisely. */
            Dwarf_Unsigned size = 0;

            /*  Ignore the offset returned.
                Already in cc_abbrev_offset. */
            _dwarf_get_dwp_extra_offset(
                &cu_context->cc_dwp_offsets,
                DW_SECT_ABBREV,&size);
            /*  ASSERT: size != 0 */
            end_abbrev_ptr = abbrev_ptr + size;
        } else {
            end_abbrev_ptr = dbg->de_debug_abbrev.dss_data +
                dbg->de_debug_abbrev.dss_size;
        }
    }

    /*  End of abbrev's as we are past the end entirely.
        This can happen,though it seems wrong.
        Or we are at the end of the data block,
        which we also take as
        meaning done with abbrevs for this CU. An abbreviations table
        is supposed to end with a zero byte. Not ended by end
        of data block.  But we are allowing what is possibly a bit
        more flexible end policy here. */
    if (abbrev_ptr >= end_abbrev_ptr) {
        return DW_DLV_NO_ENTRY;
    }
    /*  End of abbrev's for this cu, since abbrev code is 0. */
    if (*abbrev_ptr == 0) {
        return DW_DLV_NO_ENTRY;
    }

    do {
        unsigned new_hashable_val = 0;
        Dwarf_Off  abb_goff = 0;
        Dwarf_Unsigned atcount = 0;
        Dwarf_Byte_Ptr abbrev_ptr2 = 0;
        int res = 0;

        abb_goff = abbrev_ptr - dbg->de_debug_abbrev.dss_data;
        DECODE_LEB128_UWORD_CK(abbrev_ptr, abbrev_code,
            dbg,error,end_abbrev_ptr);
        DECODE_LEB128_UWORD_CK(abbrev_ptr, abbrev_tag,
            dbg,error,end_abbrev_ptr);
        if (abbrev_tag > DW_TAG_hi_user) {
            _dwarf_error(dbg, error,DW_DLE_TAG_CORRUPT);
            return DW_DLV_ERROR;
        }

        if (abbrev_ptr >= end_abbrev_ptr) {
            _dwarf_error(dbg, error, DW_DLE_ABBREV_OFF_END);
            return DW_DLV_ERROR;
        }

        inner_list_entry = (Dwarf_Abbrev_List)
            _dwarf_get_alloc(cu_context->cc_dbg, DW_DLA_ABBREV_LIST, 1);
        if (inner_list_entry == NULL) {
            _dwarf_error(dbg, error, DW_DLE_ALLOC_FAIL);
            return DW_DLV_ERROR;
        }

        new_hashable_val = abbrev_code;
        hash_num = new_hashable_val %
            hash_table_base->tb_table_entry_count;
        inner_hash_entry = entry_base + hash_num;
        /* Move_entry_to_new_hash */
        inner_list_entry->abl_next = inner_hash_entry->at_head;
        inner_hash_entry->at_head = inner_list_entry;

        hash_table_base->tb_total_abbrev_count++;

        inner_list_entry->abl_code = abbrev_code;
        inner_list_entry->abl_tag = abbrev_tag;
        inner_list_entry->abl_has_child = *(abbrev_ptr++);
        inner_list_entry->abl_abbrev_ptr = abbrev_ptr;
        inner_list_entry->abl_goffset =  abb_goff;
        hash_table_base->tb_total_abbrev_count++;

        /*  Cycle thru the abbrev content, ignoring the content except
            to find the end of the content. */
        res = _dwarf_count_abbrev_entries(dbg,abbrev_ptr,
            end_abbrev_ptr,&atcount,&abbrev_ptr2,error);
        if (res != DW_DLV_OK) {
            return res;
        }
        abbrev_ptr = abbrev_ptr2;
        inner_list_entry->abl_count = atcount;
    } while ((abbrev_ptr < end_abbrev_ptr) &&
        *abbrev_ptr != 0 && abbrev_code != code);

    cu_context->cc_last_abbrev_ptr = abbrev_ptr;
    cu_context->cc_last_abbrev_endptr = end_abbrev_ptr;
    if(abbrev_code == code) {
        *list_out = inner_list_entry;
        return DW_DLV_OK;
    }
    /*  We cannot find an abbrev_code  matching code. ERROR
        will be declared eventually.  Might be better to declare
        specific errors here? */
    return DW_DLV_NO_ENTRY;
}


/*
    We check that:
        areaptr <= strptr.
        a NUL byte (*p) exists at p < end.
    and return DW_DLV_ERROR if a check fails.

    de_assume_string_in_bounds
*/
int
_dwarf_check_string_valid(Dwarf_Debug dbg,void *areaptr,
    void *strptr, void *areaendptr,
    int suggested_error,
    Dwarf_Error*error)
{
    Dwarf_Small *start = areaptr;
    Dwarf_Small *p = strptr;
    Dwarf_Small *end = areaendptr;

    if (p < start) {
        _dwarf_error(dbg,error,suggested_error);
        return DW_DLV_ERROR;
    }
    if (p >= end) {
        _dwarf_error(dbg,error,suggested_error);
        return DW_DLV_ERROR;
    }
    if (dbg->de_assume_string_in_bounds) {
        /* This NOT the default. But folks can choose
            to live dangerously and just assume strings ok. */
        return DW_DLV_OK;
    }
    while (p < end) {
        if (*p == 0) {
            return DW_DLV_OK;
        }
        ++p;
    }
    _dwarf_error(dbg,error,DW_DLE_STRING_NOT_TERMINATED);
    return DW_DLV_ERROR;
}


/*  Return non-zero if the start/end are not valid for the
    die's section.
    If pastend matches the dss_data+dss_size then
    pastend is a pointer that cannot be dereferenced.
    But we allow it as valid here, it is normal for
    a pointer to point one-past-end in
    various circumstances (one must
    avoid dereferencing it, of course).
    Return 0 if valid. Return 1 if invalid. */
int
_dwarf_reference_outside_section(Dwarf_Die die,
    Dwarf_Small * startaddr,
    Dwarf_Small * pastend)
{
    Dwarf_Debug dbg = 0;
    Dwarf_CU_Context contxt = 0;
    struct Dwarf_Section_s *sec = 0;

    contxt = die->di_cu_context;
    dbg = contxt->cc_dbg;
    if (die->di_is_info) {
        sec = &dbg->de_debug_info;
    } else {
        sec = &dbg->de_debug_types;
    }
    if (startaddr < sec->dss_data) {
        return 1;
    }
    if (pastend > (sec->dss_data + sec->dss_size)) {
        return 1;
    }
    return 0;
}


/*
  A byte-swapping version of memcpy
  for cross-endian use.
  Only 2,4,8 should be lengths passed in.
*/
void
_dwarf_memcpy_noswap_bytes(void *s1, const void *s2, unsigned long len)
{
    memcpy(s1,s2,(size_t)len);
    return;
}
void
_dwarf_memcpy_swap_bytes(void *s1, const void *s2, unsigned long len)
{
    unsigned char *targ = (unsigned char *) s1;
    const unsigned char *src = (const unsigned char *) s2;

    if (len == 4) {
        targ[3] = src[0];
        targ[2] = src[1];
        targ[1] = src[2];
        targ[0] = src[3];
    } else if (len == 8) {
        targ[7] = src[0];
        targ[6] = src[1];
        targ[5] = src[2];
        targ[4] = src[3];
        targ[3] = src[4];
        targ[2] = src[5];
        targ[1] = src[6];
        targ[0] = src[7];
    } else if (len == 2) {
        targ[1] = src[0];
        targ[0] = src[1];
    }
/* should NOT get below here: is not the intended use */
    else if (len == 1) {
        targ[0] = src[0];
    } else {
        memcpy(s1, s2, (size_t)len);
    }
    return;
}


/*  This calculation used to be sprinkled all over.
    Now brought to one place.

    We try to accurately compute the size of a cu header
    given a known cu header location ( an offset in .debug_info
    or debug_types).  */
/* ARGSUSED */

int
_dwarf_length_of_cu_header(Dwarf_Debug dbg,
    Dwarf_Unsigned offset,
    Dwarf_Bool is_info,
    Dwarf_Unsigned *area_length_out,
    Dwarf_Error *error)
{
    int local_length_size = 0;
    int local_extension_size = 0;
    Dwarf_Half version = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Unsigned final_size = 0;
    Dwarf_Small *section_start =
        is_info? dbg->de_debug_info.dss_data:
            dbg->de_debug_types.dss_data;
    Dwarf_Small *cuptr = section_start + offset;
    Dwarf_Unsigned section_length =
        is_info? dbg->de_debug_info.dss_size:
            dbg->de_debug_types.dss_size;
    Dwarf_Small * section_end_ptr =
        section_start + section_length;

    READ_AREA_LENGTH_CK(dbg, length, Dwarf_Unsigned,
        cuptr, local_length_size, local_extension_size,
        error,section_length,section_end_ptr);

    READ_UNALIGNED_CK(dbg, version, Dwarf_Half,
        cuptr, DWARF_HALF_SIZE,error,section_end_ptr);
    cuptr += DWARF_HALF_SIZE;
    if (version == 5) {
        Dwarf_Ubyte unit_type = 0;

        READ_UNALIGNED_CK(dbg, unit_type, Dwarf_Ubyte,
            cuptr, sizeof(Dwarf_Ubyte),error,section_end_ptr);
        switch (unit_type) {
        case DW_UT_compile:
            final_size = local_extension_size +
                local_length_size  + /* Size of cu length field. */
                DWARF_HALF_SIZE + /* Size of version stamp field. */
                sizeof(Dwarf_Small)+ /* Size of  unit type field. */
                sizeof(Dwarf_Small)+ /* Size of address size field. */
                local_length_size ;  /* Size of abbrev offset field. */
            break;
        case DW_UT_type:
        case DW_UT_partial:
        case DW_UT_skeleton:
        case DW_UT_split_compile:
        case DW_UT_split_type:
        default:
            _dwarf_error(dbg,error,DW_DLE_UNIT_TYPE_NOT_HANDLED);
            return DW_DLV_ERROR;
        }
    } else if (version == 4) {
        final_size = local_extension_size +
            local_length_size  +  /* Size of cu length field. */
            DWARF_HALF_SIZE +  /* Size of version stamp field. */
            local_length_size  +  /* Size of abbrev offset field. */
            sizeof(Dwarf_Small);  /* Size of address size field. */
        if (!is_info) {
            final_size +=
            /* type signature size */
            sizeof (Dwarf_Sig8) +
            /* type offset size */
            local_length_size;
        }
    } else if (version < 4) {
        final_size = local_extension_size +
            local_length_size  +
            DWARF_HALF_SIZE +
            local_length_size  +
            sizeof(Dwarf_Small);  /* Size of address size field. */
    }

    *area_length_out = final_size;
    return DW_DLV_OK;
}

/*  Pretend we know nothing about the CU
    and just roughly compute the result.  */
Dwarf_Unsigned
_dwarf_length_of_cu_header_simple(Dwarf_Debug dbg,
    Dwarf_Bool dinfo)
{
    Dwarf_Unsigned finalsize = 0;
    finalsize =  dbg->de_length_size + /* Size of cu length field. */
        DWARF_HALF_SIZE +    /* Size of version stamp field. */
        dbg->de_length_size +   /* Size of abbrev offset field. */
        sizeof(Dwarf_Small);    /* Size of address size field. */
    if (!dinfo) {
        finalsize +=
            /* type signature size */
            sizeof (Dwarf_Sig8) +
            /* type offset size */
            dbg->de_length_size;
    }
    return finalsize;
}

/*  Now that we delay loading .debug_info, we need to do the
    load in more places. So putting the load
    code in one place now instead of replicating it in multiple
    places.  */
int
_dwarf_load_debug_info(Dwarf_Debug dbg, Dwarf_Error * error)
{
    int res = DW_DLV_ERROR;
    if (dbg->de_debug_info.dss_data) {
        return DW_DLV_OK;
    }
    res = _dwarf_load_section(dbg, &dbg->de_debug_abbrev,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_load_section(dbg, &dbg->de_debug_info, error);
    return res;
}
int
_dwarf_load_debug_types(Dwarf_Debug dbg, Dwarf_Error * error)
{
    int res = DW_DLV_ERROR;
    if (dbg->de_debug_types.dss_data) {
        return DW_DLV_OK;
    }
    res = _dwarf_load_section(dbg, &dbg->de_debug_abbrev,error);
    if (res != DW_DLV_OK) {
        return res;
    }
    res = _dwarf_load_section(dbg, &dbg->de_debug_types, error);
    return res;
}
void
_dwarf_free_abbrev_hash_table_contents(Dwarf_Debug dbg,Dwarf_Hash_Table hash_table)
{
    /*  A Hash Table is an array with tb_table_entry_count struct
        Dwarf_Hash_Table_s entries in the array. */
    unsigned hashnum = 0;
    for (; hashnum < hash_table->tb_table_entry_count; ++hashnum) {
        struct Dwarf_Abbrev_List_s *abbrev = 0;
        struct Dwarf_Abbrev_List_s *nextabbrev = 0;
        struct  Dwarf_Hash_Table_Entry_s *tb =  &hash_table->tb_entries[hashnum];

        abbrev = tb->at_head;
        for (; abbrev; abbrev = nextabbrev) {
            nextabbrev = abbrev->abl_next;
            abbrev->abl_next = 0;
            dwarf_dealloc(dbg, abbrev, DW_DLA_ABBREV_LIST);
        }
        tb->at_head = 0;
    }
    /* Frees all the entries at once: an array. */
    dwarf_dealloc(dbg,hash_table->tb_entries,DW_DLA_HASH_TABLE_ENTRY);
    hash_table->tb_entries = 0;
}

/*
    If no die provided the size value returned might be wrong.
    If different compilation units have different address sizes
    this may not give the correct value in all contexts if the die
    pointer is NULL.
    If the Elf offset size != address_size
    (for example if address_size = 4 but recorded in elf64 object)
    this may not give the correct value in all contexts if the die
    pointer is NULL.
    If the die pointer is non-NULL (in which case it must point to
    a valid DIE) this will return the correct size.
*/
int
_dwarf_get_address_size(Dwarf_Debug dbg, Dwarf_Die die)
{
    Dwarf_CU_Context context = 0;
    Dwarf_Half addrsize = 0;
    if (!die) {
        return dbg->de_pointer_size;
    }
    context = die->di_cu_context;
    addrsize = context->cc_address_size;
    return addrsize;
}

/* Encode val as an unsigned LEB128. */
int dwarf_encode_leb128(Dwarf_Unsigned val, int *nbytes,
    char *space, int splen)
{
    /* Encode val as an unsigned LEB128. */
    return _dwarf_pro_encode_leb128_nm(val,nbytes,space,splen);
}

/* Encode val as a signed LEB128. */
int dwarf_encode_signed_leb128(Dwarf_Signed val, int *nbytes,
    char *space, int splen)
{
    /* Encode val as a signed LEB128. */
    return _dwarf_pro_encode_signed_leb128_nm(val,nbytes,space,splen);
}


struct  Dwarf_Printf_Callback_Info_s
dwarf_register_printf_callback( Dwarf_Debug dbg,
    struct  Dwarf_Printf_Callback_Info_s * newvalues)
{
    struct  Dwarf_Printf_Callback_Info_s oldval = dbg->de_printf_callback;
    if (!newvalues) {
        return oldval;
    }
    if( newvalues->dp_buffer_user_provided) {
        if( oldval.dp_buffer_user_provided) {
            /* User continues to control the buffer. */
            dbg->de_printf_callback = *newvalues;
        }else {
            /*  Switch from our control of buffer to user
                control.  */
            free(oldval.dp_buffer);
            oldval.dp_buffer = 0;
            dbg->de_printf_callback = *newvalues;
        }
    } else if (oldval.dp_buffer_user_provided){
        /* Switch from user control to our control */
        dbg->de_printf_callback = *newvalues;
        dbg->de_printf_callback.dp_buffer_len = 0;
        dbg->de_printf_callback.dp_buffer= 0;
    } else {
        /* User does not control the buffer. */
        dbg->de_printf_callback = *newvalues;
        dbg->de_printf_callback.dp_buffer_len =
            oldval.dp_buffer_len;
        dbg->de_printf_callback.dp_buffer =
            oldval.dp_buffer;
    }
    return oldval;
}


/*  Allocate a bigger buffer if necessary.
    Do not worry about previous content of the buffer.
    Return 0 if we fail here.
    Else return the requested len value. */
static unsigned buffersetsize(Dwarf_Debug dbg,
    struct  Dwarf_Printf_Callback_Info_s *bufdata,
    int len)
{
    char *space = 0;

    if (!dbg->de_printf_callback_null_device_handle) {
        FILE *de = fopen(NULL_DEVICE_NAME,"w");
        if(!de) {
            return 0;
        }
        dbg->de_printf_callback_null_device_handle = de;
    }
    if (bufdata->dp_buffer_user_provided) {
        return bufdata->dp_buffer_len;
    }
    /* Make big enough for a trailing NUL char. */
    space = (char *)malloc(len+1);
    if (!space) {
        /* Out of space, we cannot do anything. */
        return 0;
    }
    free(bufdata->dp_buffer);
    bufdata->dp_buffer = space;
    bufdata->dp_buffer_len = len;
    return len;
}

/*  We are only using C90 facilities, not C99,
    in libdwarf/dwarfdump. */
int
dwarf_printf(Dwarf_Debug dbg,
    const char * format,
    ...)
{
    va_list ap;
    unsigned bff = 0;
    struct Dwarf_Printf_Callback_Info_s *bufdata =
        &dbg->de_printf_callback;
    FILE * null_device_handle = 0;

    dwarf_printf_callback_function_type func = bufdata->dp_fptr;
    if (!func) {
        return 0;
    }
    null_device_handle =
        (FILE *) dbg->de_printf_callback_null_device_handle;
    if (!bufdata->dp_buffer || !null_device_handle) {
        /*  Sets dbg device handle for later use if not
            set already. */
        bff = buffersetsize(dbg,bufdata,MINBUFLEN);
        if (!bff) {
            /*  Something is wrong. */
            return 0;
        }
        if (!bufdata->dp_buffer) {
            /*  Something is wrong. Possibly caller
                set up callback wrong. */
            return 0;
        }
    }

    {
        int plen = 0;
        int nlen = 0;
        null_device_handle =
            (FILE *) dbg->de_printf_callback_null_device_handle;
        if (!null_device_handle) {
            /*  Something is wrong. */
            return 0;
        }
        va_start(ap,format);
        plen = vfprintf(null_device_handle,format,ap);
        va_end(ap);

        if (!bufdata->dp_buffer_user_provided) {
            if (plen >= (int)bufdata->dp_buffer_len) {
                bff = buffersetsize(dbg,bufdata,plen+2);
                if (!bff) {
                    /*  Something is wrong.  */
                    return 0;
                }
            }
        } else {
            if (plen >= (int)bufdata->dp_buffer_len) {
                /*  We are stuck! User did not
                    give us space needed!  */
                return 0;
            }
        }

        va_start(ap,format);
        nlen = vsprintf(bufdata->dp_buffer,
            format,ap);
        va_end(ap);
        if ( nlen > plen) {
            /* Impossible. Memory is corrupted now */
            fprintf(stderr,"\nlibdwarf impossible sprintf error %s %d\n",
                __FILE__,__LINE__);
            exit(1);
        }
        func(bufdata->dp_user_pointer,bufdata->dp_buffer);
        return nlen;
    }
    /* Not reached. */
    return 0;
}

/*  Often errs and errt point to the same Dwarf_Error,
    So exercise care.
    All the arguments MUST be non-null.*/
void
_dwarf_error_mv_s_to_t(Dwarf_Debug dbgs,Dwarf_Error *errs,
    Dwarf_Debug dbgt,Dwarf_Error *errt)
{
    if (!errt || !errs) {
        return;
    }
    if (!dbgs || !dbgt) {
        return;
    }
    if(dbgs == dbgt) {
        if(errs != errt) {
            Dwarf_Error ers = *errs;
            *errs = 0;
            *errt = ers;
        }
    } else {
        /*  Do not stomp on the system errno
            variable if there is one! */
        int mydw_errno = dwarf_errno(*errs);

        dwarf_dealloc(dbgs,*errs, DW_DLA_ERROR);
        *errs = 0;
        _dwarf_error(dbgt,errt, mydw_errno);
    }
}

static int
inthissection(struct Dwarf_Section_s *sec,Dwarf_Small *ptr)
{
    if (!sec->dss_data) {
        return FALSE;
    }
    if (ptr < sec->dss_data ) {
        return FALSE;
    }
    if (ptr >= (sec->dss_data + sec->dss_size) ) {
        return FALSE;
    }
    return TRUE;
}

#define FINDSEC(m_s,m_p,n,st,l,e)    \
do {                                 \
    if (inthissection((m_s),(m_p))) { \
        *(n) = (m_s)->dss_name;      \
        *(st)= (m_s)->dss_data;      \
        *(l) = (m_s)->dss_size;      \
        *(e) = (m_s)->dss_data + (m_s)->dss_size; \
        return DW_DLV_OK;            \
    }                                \
} while (0)


/* So we can know a section end even when we do not
    have the section info apriori  It's only
    needed for a subset of sections. */
int
_dwarf_what_section_are_we(Dwarf_Debug dbg,
    Dwarf_Small    *  our_pointer,
    const char     ** section_name_out,
    Dwarf_Small    ** sec_start_ptr_out,
    Dwarf_Unsigned *  sec_len_out,
    Dwarf_Small    ** sec_end_ptr_out,
    UNUSEDARG Dwarf_Error    *  error)
{
    FINDSEC(&dbg->de_debug_info,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_loc,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_line,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_aranges,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_macro,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_ranges,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_str_offsets,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_addr,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_pubtypes,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_gdbindex,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_abbrev,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_cu_index,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_tu_index,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_line_str,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_types,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_sup,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_frame,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    FINDSEC(&dbg->de_debug_frame_eh_gnu,
        our_pointer, section_name_out,
        sec_start_ptr_out, sec_len_out, sec_end_ptr_out);
    return DW_DLV_NO_ENTRY;
}

static int
does_file_exist(char *f)
{
    int fd = 0;

    fd = open(f,O_RDONLY|O_BINARY);
    if (fd < 0) {
        return DW_DLV_NO_ENTRY;
    }
    /* Here we could derive the crc to validate the file. */
    close(fd);
    return DW_DLV_OK;
}

struct joins_s {
    char * js_fullpath;
    char * js_dirname;
    size_t js_dirnamelen;
    char * js_wd;
    size_t js_wdlen;
    char * js_wd2;
    size_t js_wd2len;
    char * js_tname;
    char * js_originalfullpath;
    size_t js_originalfullpathlen;
};

static void
construct_js(struct joins_s * js)
{
    memset(js,0,sizeof(struct joins_s));
}
static void
destruct_js(struct joins_s * js)
{
    free(js->js_fullpath);
    js->js_fullpath = 0;

    free(js->js_dirname);
    js->js_dirname = 0;
    js->js_dirnamelen = 0;

    free(js->js_wd);
    js->js_wd = 0;
    js->js_wdlen = 0;

    free(js->js_wd2);
    js->js_wd2 = 0;
    js->js_wd2len = 0;

    free(js->js_tname);
    js->js_tname = 0;

    free(js->js_originalfullpath);
    js->js_originalfullpathlen = 0;
}

static char joinchar = '/';
static int
pathjoinl(char *target, size_t tsize,char *input)
{
    size_t targused = strlen(target);
    size_t inputsize = strlen(input);
    if (!input) {
        /* Nothing to do. Ok. */
        return DW_DLV_OK;
    }
    if ((targused+inputsize+3) > tsize) {
        return DW_DLV_ERROR;
    }

    if (!*target) {
        if (*input != joinchar) {
            target[0] = joinchar;
            strcpy(target+1,input);
        } else {
            strcpy(target,input);
        }
    }
    if (target[targused-1] != joinchar) {
        if (*input != joinchar) {
            target[targused] = joinchar;
            strcpy(target+targused+1,input);
        } else {
            strcpy(target+targused,input);
        }
    } else {
        if (*input != joinchar) {
            strcpy(target+targused,input);
        } else {
            strcpy(target+targused,input+1);
        }
    }
    return DW_DLV_OK;
}
/*  ASSERT: the last character in s is not a /  */
static size_t
mydirlen(char *s)
{
    char *cp = 0;
    char *lastjoinchar = 0;
    size_t count =0;

    for(cp = s ; *cp ; ++cp,++count)  {
        if (*cp == joinchar) {
            lastjoinchar = cp;
        }
    }
    if (lastjoinchar) {
        /* we know diff is postive in all cases */
        ptrdiff_t diff =  lastjoinchar - s;
        /* count the last join charn mydirlen. */
        return (size_t)(diff+1);
    }
    return 0;
}


/*  New September 2019.  Access to the GNU section named
    .gnu_debuglink
    See
    https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html
*/
static void
construct_linkedto_path(Dwarf_Debug dbg,
   char * ptr,
   char ** debuglink_out,
   unsigned * debuglink_length)
{
    char * depath = (char *)dbg->de_path;
    size_t depathlen = strlen(depath)+1;
    size_t joinbaselen = 0;
    char * basename = 0;
    size_t basenamelen = 0;
    size_t wdretlen = 0;
    size_t buflen =2000;
    size_t maxlen = 0;
    char * tname = 0;
    int res = 0;
    struct joins_s joind;

    construct_js(&joind);
    joind.js_dirnamelen = mydirlen(depath);
    if (joind.js_dirnamelen) {
        joind.js_dirname = malloc(depathlen);
        if(!joind.js_dirname) {
            *debuglink_length = 0;
            destruct_js(&joind);
            return;
        }
        joind.js_dirname[0] = 0;
        strncpy(joind.js_dirname,depath,joind.js_dirnamelen);
        joind.js_dirname[joind.js_dirnamelen] = 0;
    }
    basename = ptr;
    basenamelen = strlen(basename);

    joind.js_wdlen = buflen + depathlen + basenamelen +100;
    joind.js_wd = malloc(joind.js_wdlen);
    if(!joind.js_wd) {
        *debuglink_length = 0;
        destruct_js(&joind);
        return;
    }
    joind.js_wd[0] = 0;
    if (depath[0] != joinchar) {
        char *wdret = 0;
        wdret = getcwd(joind.js_wd,joind.js_wdlen);
        if (!wdret) {
            destruct_js(&joind);
            *debuglink_length = 0;
            return;
        }
        wdretlen = strlen(joind.js_wd);
    }
    /* We know the default places plus slashes won't add 100 */
    maxlen = wdretlen + depathlen +100 + basenamelen;
    if ((maxlen > joind.js_wdlen) && wdretlen) {
        char *newwd = 0;

        newwd = malloc(maxlen);
        if(!newwd) {
            destruct_js(&joind);
            *debuglink_length = 0;
            return;
        }
        newwd[0] = 0;
        strcpy(newwd,joind.js_wd);
        free(joind.js_wd);
        joind.js_wd = newwd;
        joind.js_wdlen = maxlen;
    }
    {
        char *opath = 0;

        opath = malloc(maxlen);
        if(!opath) {
            destruct_js(&joind);
            *debuglink_length = 0;
            return;
        }
        opath[0] = 0;
        joind.js_originalfullpath = opath;
        joind.js_originalfullpathlen = maxlen;
        opath = 0;
        strcpy(joind.js_originalfullpath,joind.js_wd);
        res =  pathjoinl(joind.js_originalfullpath,joind.js_originalfullpathlen,
            depath);
        if (res != DW_DLV_OK) {
            destruct_js(&joind);
            *debuglink_length = 0;
            return;
        }
    }
    /*  We need to be sure there is no accidental match with the file we opened. */
    /* wd suffices to build strings. */
    if (joind.js_dirname) {
        res = pathjoinl(joind.js_wd,joind.js_wdlen,
            joind.js_dirname);
    } else {
        res = DW_DLV_OK;
    }
    /* Now js_wd is a leading / directory name. */
    joinbaselen = strlen(joind.js_wd);
    if (res == DW_DLV_OK) {
        /* If we add basename do we find what we look for? */
        res = pathjoinl(joind.js_wd,joind.js_wdlen,basename);
        if (!strcmp(joind.js_originalfullpath,joind.js_wd)) {
            /* duplicated name. spurious match. */
        } else if (res == DW_DLV_OK) {
            res = does_file_exist(joind.js_wd);
            if (res == DW_DLV_OK) {
                *debuglink_out = joind.js_wd;
                *debuglink_length = strlen(joind.js_wd);
                /* ownership passed to caller, then free the rest */
                joind.js_wd = 0;
                joind.js_wdlen = 0;
                destruct_js(&joind);
                return;
            }
        }
    }
    /* No, so remove the basename */
    joind.js_wd[joinbaselen] = 0;
    tname = ".debug";
    res = pathjoinl(joind.js_wd,joind.js_wdlen,tname);
    if (res == DW_DLV_OK) {
        res = pathjoinl(joind.js_wd,joind.js_wdlen,basename);
        if (!strcmp(joind.js_originalfullpath,joind.js_wd)) {
            /* duplicated name. spurious match. */
        } else if(res == DW_DLV_OK) {
            res = does_file_exist(joind.js_wd);
            if (res == DW_DLV_OK) {
                *debuglink_out = joind.js_wd;
                *debuglink_length = strlen(joind.js_wd);
                /* ownership passed to caller, then free the rest */
                joind.js_wd = 0;
                joind.js_wdlen = 0;
                destruct_js(&joind);
                return;
            }
        }
    }
    /* Not found, so remove the .debug etc */
    joind.js_wd[joinbaselen] = 0;

    tname = "/usr/lib/debug";
    joind.js_wd2 = malloc(maxlen);
    if(!joind.js_wd2) {
        *debuglink_length = 0;
        destruct_js(&joind);
        return;
    }
    joind.js_wd2len = maxlen;
    joind.js_wd2[0] = 0;
    strcpy(joind.js_wd2,tname);
    res = pathjoinl(joind.js_wd2,joind.js_wd2len,joind.js_wd);
    if (res == DW_DLV_OK) {
        res = pathjoinl(joind.js_wd2,joind.js_wd2len,basename);
        if (!strcmp(joind.js_originalfullpath,joind.js_wd2)) {
            /* duplicated name. spurious match. */
        } else if (res == DW_DLV_OK) {
            res = does_file_exist(joind.js_wd2);
            if (res == DW_DLV_OK) {
                *debuglink_out = joind.js_wd2;
                *debuglink_length = strlen(joind.js_wd2);
                /* ownership passed to caller, then free the rest */
                joind.js_wd2 = 0;
                joind.js_wd2len = 0;
                destruct_js(&joind);
                return;
            }
        }
    }
    *debuglink_length = 0;
    destruct_js(&joind);
    return;
}

int
dwarf_gnu_debuglink(Dwarf_Debug dbg,
    char ** name_returned,  /* static storage, do not free */
    char ** crc_returned, /* 32bit crc , do not free */
    char **  debuglink_path_returned, /* caller must free returned pointer */
    unsigned *debuglink_path_size_returned,/* Size of the debuglink path.
        zero returned if no path known/found. */
    Dwarf_Error*   error)
{
    char *ptr = 0;
    char *endptr = 0;
    unsigned namelen = 0;
    unsigned m = 0;
    unsigned incr = 0;
    char *crcptr = 0;
    int res = DW_DLV_ERROR;

    if (!dbg->de_gnu_debuglink.dss_data) {
        res = _dwarf_load_section(dbg,
            &dbg->de_gnu_debuglink,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    ptr = (char *)dbg->de_gnu_debuglink.dss_data;
    endptr = ptr + dbg->de_gnu_debuglink.dss_size;
    res = _dwarf_check_string_valid(dbg,ptr,
        ptr,
        endptr,
        DW_DLE_FORM_STRING_BAD_STRING,
        error);
    if (res != DW_DLV_OK) {
        return res;
    }
    namelen = (unsigned)strlen((const char*)ptr);
    m = (namelen+1) %4;
    if (m) {
        incr = 4 - m;
    }
    crcptr = ptr +namelen +1 +incr;
    if ((crcptr +4) != endptr) {
        _dwarf_error(dbg,error,DW_DLE_CORRUPT_GNU_DEBUGLINK);
        return DW_DLV_ERROR;
    }
    if (dbg->de_path) {
        construct_linkedto_path(dbg,ptr,debuglink_path_returned,
            debuglink_path_size_returned);
    } else {
        *debuglink_path_size_returned  = 0;
    }
    *name_returned = ptr;
    *crc_returned = crcptr;
    return DW_DLV_OK;
}

/* New September 2019. */
int  dwarf_add_file_path(
    UNUSEDARG Dwarf_Debug dbg,
    const char *          file_name,
    UNUSEDARG Dwarf_Error* error)
{
    if (!dbg->de_path) {
        dbg->de_path = strdup(file_name);
    }
    return DW_DLV_OK;
}


/*  The definition of .note.gnu.buildid contents (also
    used for other GNU .note.gnu.  sections too. */
struct buildid_s {
    char bu_ownernamesize[4];
    char bu_buildidsize[4];
    char bu_type[4];
    char bu_owner[1];
};

int
dwarf_gnu_buildid(Dwarf_Debug dbg,
    Dwarf_Unsigned * type_returned,
    const char     **owner_name_returned,
    Dwarf_Unsigned * build_id_length_returned,
    const unsigned char  **build_id_returned,
    Dwarf_Error*   error)
{
    Dwarf_Byte_Ptr ptr = 0;
    Dwarf_Byte_Ptr endptr = 0;
    int res = DW_DLV_ERROR;
    struct buildid_s *bu = 0;
    Dwarf_Unsigned namesize = 0;
    Dwarf_Unsigned descrsize = 0;
    Dwarf_Unsigned type = 0;

    if (!dbg->de_note_gnu_buildid.dss_data) {
        res = _dwarf_load_section(dbg,
            &dbg->de_note_gnu_buildid,error);
        if (res != DW_DLV_OK) {
            return res;
        }
    }
    ptr = (Dwarf_Byte_Ptr)dbg->de_note_gnu_buildid.dss_data;
    endptr = ptr + dbg->de_note_gnu_buildid.dss_size;

    if (dbg->de_note_gnu_buildid.dss_size <
        sizeof(struct buildid_s)) {
        _dwarf_error(dbg,error,
            DW_DLE_CORRUPT_NOTE_GNU_DEBUGID);
        return DW_DLV_ERROR;
    }

    bu = (struct buildid_s *)ptr;
    READ_UNALIGNED_CK(dbg,namesize,Dwarf_Unsigned,
        (Dwarf_Byte_Ptr)&bu->bu_ownernamesize[0], 4,
        error,endptr);
    READ_UNALIGNED_CK(dbg,descrsize,Dwarf_Unsigned,
        (Dwarf_Byte_Ptr)&bu->bu_buildidsize[0], 4,
        error,endptr);
    READ_UNALIGNED_CK(dbg,type,Dwarf_Unsigned,
        (Dwarf_Byte_Ptr)&bu->bu_type[0], 4,
        error,endptr);

    if (descrsize != 20) {
        _dwarf_error(dbg,error,DW_DLE_CORRUPT_NOTE_GNU_DEBUGID);
        return DW_DLV_ERROR;
    }
    res = _dwarf_check_string_valid(dbg,&bu->bu_owner[0],
        &bu->bu_owner[0],
        endptr,
        DW_DLE_CORRUPT_GNU_DEBUGID_STRING,
        error);
    if ( res != DW_DLV_OK) {
        return res;
    }
    if ((strlen(bu->bu_owner) +1) != namesize) {
        _dwarf_error(dbg,error,DW_DLE_CORRUPT_GNU_DEBUGID_STRING);
        return DW_DLV_ERROR;
    }

    if ((sizeof(struct buildid_s)-1 + namesize + descrsize) >
        dbg->de_note_gnu_buildid.dss_size) {
        _dwarf_error(dbg,error,DW_DLE_CORRUPT_GNU_DEBUGID_SIZE);
        return DW_DLV_ERROR;
    }
    *type_returned = type;
    *owner_name_returned = &bu->bu_owner[0];
    *build_id_length_returned = descrsize;
    *build_id_returned = ptr + sizeof(struct buildid_s)-1 + namesize;
    return DW_DLV_OK;
}
