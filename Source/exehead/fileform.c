/*
 * fileform.c
 * 
 * This file is a part of NSIS.
 * 
 * Copyright (C) 1999-2009 Nullsoft and Contributors
 * 
 * Licensed under the zlib/libpng license (the "License");
 * you may not use this file except in compliance with the License.
 * 
 * Licence details can be found in the file COPYING.
 * 
 * This software is provided 'as-is', without any express or implied
 * warranty.
 *
 * Unicode support by Jim Park -- 08/13/2007
 */

#include "../Platform.h"
#include "fileform.h"
#include "util.h"
#include "state.h"
#include "resource.h"
#include "lang.h"
#include "ui.h"
#include "exec.h"
#include "../crc32.h"
#include "../tchar.h"
#include <assert.h>

#ifdef NSIS_CONFIG_COMPRESSION_SUPPORT
#ifdef NSIS_COMPRESS_USE_ZLIB
#include "../zlib/ZLIB.H"
#endif

#ifdef NSIS_COMPRESS_USE_LZMA
#include "../7zip/LZMADecode.h"
#define z_stream lzma_stream
#define inflateInit(x) lzmaInit(x)
#define inflateReset(x) lzmaInit(x)
#define inflate(x) lzmaDecode(x)
#define Z_OK LZMA_OK
#define Z_STREAM_END LZMA_STREAM_END
#endif

#ifdef NSIS_COMPRESS_USE_BZIP2
#include "../bzip2/bzlib.h"

#define z_stream DState
#define inflateInit(x) BZ2_bzDecompressInit(x)
#define inflateReset(x) BZ2_bzDecompressInit(x)

#define inflate(x) BZ2_bzDecompress(x)
#define Z_OK BZ_OK
#define Z_STREAM_END BZ_STREAM_END
#endif//NSIS_COMPRESS_USE_BZIP2
#endif//NSIS_CONFIG_COMPRESSION_SUPPORT

struct block_header g_blocks[BLOCKS_NUM];
header *g_header;
int g_flags;
int g_filehdrsize;
int g_is_uninstaller;

HANDLE g_db_hFile=INVALID_HANDLE_VALUE;

#if defined(NSIS_CONFIG_COMPRESSION_SUPPORT) && defined(NSIS_COMPRESS_WHOLE)
HANDLE dbd_hFile=INVALID_HANDLE_VALUE;
static __int64 dbd_size, dbd_pos, dbd_srcpos, dbd_fulllen;
#endif//NSIS_COMPRESS_WHOLE

static __int64 m_length;
static __int64 m_pos;

// need a slist to maintain the file offset mapping
struct  FileMapping
{
	struct FileMapping* next;
	dataheader dh;
};
struct FileMappingHeader 
{
	struct FileMapping* first;
	struct FileMapping* cur;
	BOOL LoadFinished;
	__int64 cur_offset;
};
static struct FileMappingHeader m_file_mapping;
NSIS_STRING m_data_file_path;


#define _calc_percent() (min(m_pos,m_length)*100/m_length)
#ifdef NSIS_COMPRESS_WHOLE
static int NSISCALL calc_percent()
{
  return _calc_percent();
}
#else
#define calc_percent() _calc_percent()
#endif

#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
#if defined(NSIS_CONFIG_CRC_SUPPORT) || defined(NSIS_COMPRESS_WHOLE)
BOOL CALLBACK verProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  if (uMsg == WM_INITDIALOG)
  {
    SetTimer(hwndDlg,1,250,NULL);
    uMsg = WM_TIMER;
  }
  if (uMsg == WM_TIMER)
  {
    TCHAR bt[64];
    int percent=calc_percent();
#ifdef NSIS_COMPRESS_WHOLE
    TCHAR *msg=g_header?_LANG_UNPACKING:_LANG_VERIFYINGINST;
#else
    TCHAR *msg=_LANG_VERIFYINGINST;
#endif

    wsprintf(bt,msg,percent);

    my_SetWindowText(hwndDlg,bt);
    my_SetDialogItemText(hwndDlg,IDC_STR,bt);
  }
  return 0;
}

DWORD verify_time;

void handle_ver_dlg(BOOL kill)
{
  static HWND hwnd;

  if (kill)
  {
    if (hwnd) DestroyWindow(hwnd);
    hwnd = NULL;

    return;
  }

  if (hwnd)
  {
    MessageLoop(0);
  }
  else if (GetTickCount() > verify_time)
  {
#ifdef NSIS_COMPRESS_WHOLE
    if (g_hwnd)
    {
      if (g_exec_flags.status_update & 1)
      {
        TCHAR bt[64];
        wsprintf(bt, _T("... %d%%"), calc_percent());
        update_status_text(0, bt);
      }
    }
    else
#endif
    {
      hwnd = CreateDialog(
        g_hInstance,
        MAKEINTRESOURCE(IDD_VERIFY),
        0,
        verProc
      );
      ShowWindow(hwnd, SW_SHOW);
    }
  }
}

#endif//NSIS_CONFIG_CRC_SUPPORT || NSIS_COMPRESS_WHOLE
#endif//NSIS_CONFIG_VISIBLE_SUPPORT

#ifdef NSIS_CONFIG_COMPRESSION_SUPPORT
static z_stream g_inflate_stream;
#endif

const TCHAR * NSISCALL loadHeaders(int cl_flags)
{
  __int64 left;
#ifdef NSIS_CONFIG_CRC_SUPPORT
  crc32_t crc = 0;
  int do_crc = 0;
#endif//NSIS_CONFIG_CRC_SUPPORT

  void *data;
  firstheader h;
  header *header;
  dataheader dh;

  HANDLE db_hFile;

#ifdef NSIS_CONFIG_CRC_SUPPORT
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
  verify_time = GetTickCount() + 1000;
#endif
#endif//NSIS_CONFIG_CRC_SUPPORT

  GetModuleFileName(NULL, state_exe_path, NSIS_MAX_STRLEN);

  g_db_hFile = db_hFile = myOpenFile(state_exe_path, GENERIC_READ, OPEN_EXISTING);
  if (db_hFile == INVALID_HANDLE_VALUE)
  {
    return _LANG_CANTOPENSELF;
  }

  mystrcpy(state_exe_directory, state_exe_path);
  mystrcpy(state_exe_file, trimslashtoend(state_exe_directory));

  GetFileSizeEx(db_hFile, (LARGE_INTEGER*)&m_length);
  left = m_length;
  while (left > 0)
  {
    static char temp[32768*32]; // modified by yew: for fast crc
    DWORD l = min(left, (g_filehdrsize ? sizeof(temp) : 512));
    if (!ReadSelfFile(temp, l))
    {
#if defined(NSIS_CONFIG_CRC_SUPPORT) && defined(NSIS_CONFIG_VISIBLE_SUPPORT)
      handle_ver_dlg(TRUE);
#endif//NSIS_CONFIG_CRC_SUPPORT
      return _LANG_INVALIDCRC;
    }

    if (!g_filehdrsize)
    {
      mini_memcpy(&h, temp, sizeof(firstheader));
      if (
           (h.flags & (~FH_FLAGS_MASK)) == 0 &&
           h.siginfo == FH_SIG &&
           h.nsinst[2] == FH_INT3 &&
           h.nsinst[1] == FH_INT2 &&
           h.nsinst[0] == FH_INT1
         )
      {
        g_filehdrsize = m_pos;

#if defined(NSIS_CONFIG_CRC_SUPPORT) || defined(NSIS_CONFIG_SILENT_SUPPORT)
        cl_flags |= h.flags;
#endif

#ifdef NSIS_CONFIG_SILENT_SUPPORT
        g_exec_flags.silent |= cl_flags & FH_FLAGS_SILENT;
#endif

        if (h.length_of_all_following_data > left)
          return _LANG_INVALIDCRC;

#ifdef NSIS_CONFIG_CRC_SUPPORT
        if ((cl_flags & FH_FLAGS_FORCE_CRC) == 0)
        {
          if (cl_flags & FH_FLAGS_NO_CRC)
            break;
        }

        do_crc++;

#ifndef NSIS_CONFIG_CRC_ANAL
        left = h.length_of_all_following_data - 4;
        // end crc checking at crc :) this means you can tack stuff on the end and it'll still work.
#else //!NSIS_CONFIG_CRC_ANAL
        left -= 4;
#endif//NSIS_CONFIG_CRC_ANAL
        // this is in case the end part is < 512 bytes.
        if (l > left) l=left;

#else//!NSIS_CONFIG_CRC_SUPPORT
        // no crc support, no need to keep on reading
        break;
#endif//!NSIS_CONFIG_CRC_SUPPORT
      }
    }
#ifdef NSIS_CONFIG_CRC_SUPPORT

#ifdef NSIS_CONFIG_VISIBLE_SUPPORT

#ifdef NSIS_CONFIG_SILENT_SUPPORT
    else if ((cl_flags & FH_FLAGS_SILENT) == 0)
#endif//NSIS_CONFIG_SILENT_SUPPORT
    {
      handle_ver_dlg(FALSE);
    }
#endif//NSIS_CONFIG_VISIBLE_SUPPORT

#ifndef NSIS_CONFIG_CRC_ANAL
    if (left < m_length)
#endif//NSIS_CONFIG_CRC_ANAL
      crc = CRC32(crc, (unsigned char*)temp, l);

#endif//NSIS_CONFIG_CRC_SUPPORT
    m_pos += l;
    left -= l;
  }
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
#ifdef NSIS_CONFIG_CRC_SUPPORT
  handle_ver_dlg(TRUE);
#endif//NSIS_CONFIG_CRC_SUPPORT
#endif//NSIS_CONFIG_VISIBLE_SUPPORT
  if (!g_filehdrsize)
    return _LANG_INVALIDCRC;

#ifdef NSIS_CONFIG_CRC_SUPPORT
  if (do_crc)
  {
    crc32_t fcrc;
    SetSelfFilePointer(m_pos);
    if (!ReadSelfFile(&fcrc, sizeof(crc32_t)) || crc != fcrc)
      return _LANG_INVALIDCRC;
  }
#endif//NSIS_CONFIG_CRC_SUPPORT

  data = (void *)GlobalAlloc(GPTR,h.length_of_header);

#ifdef NSIS_COMPRESS_WHOLE
  inflateReset(&g_inflate_stream);

  {
    TCHAR fno[MAX_PATH];
    my_GetTempFileName(fno, state_temp_dir);
    dbd_hFile=CreateFile(fno,GENERIC_WRITE|GENERIC_READ,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    if (dbd_hFile == INVALID_HANDLE_VALUE)
      return _LANG_ERRORWRITINGTEMP;
  }
  dbd_srcpos = SetSelfFilePointer(g_filehdrsize + sizeof(firstheader));
#ifdef NSIS_CONFIG_CRC_SUPPORT
  dbd_fulllen = dbd_srcpos - sizeof(h) + h.length_of_all_following_data - ((h.flags & FH_FLAGS_NO_CRC) ? 0 : sizeof(crc32_t));
#else
  dbd_fulllen = dbd_srcpos - sizeof(h) + h.length_of_all_following_data;
#endif//NSIS_CONFIG_CRC_SUPPORT
#else
  SetSelfFilePointer(g_filehdrsize + sizeof(firstheader));
#endif//NSIS_COMPRESS_WHOLE

  if (GetCompressedDataFromDataBlockToMemory(-1, data, h.length_of_header) != h.length_of_header)
  {
    return _LANG_INVALIDCRC;
  }

  header = g_header = data;

  g_flags = header->flags;

#ifdef NSIS_CONFIG_UNINSTALL_SUPPORT
  if (h.flags & FH_FLAGS_UNINSTALL)
    g_is_uninstaller++;
#endif

  // set offsets to real memory offsets rather than installer's header offset
  left = BLOCKS_NUM;
  while (left--)
    header->blocks[left].offset += (int)data;

  m_file_mapping.first = NULL;
  m_file_mapping.cur = NULL;
  m_file_mapping.LoadFinished = FALSE;

#ifdef NSIS_COMPRESS_WHOLE
  header->blocks[NB_DATA].offset = dbd_pos;
#else
  if (h.flags & FH_FLAGS_DATA_FILE)
  {
	  LPTSTR psz,psz2;
	  int cur_index= 1;
	  __int64 length;
	  while(1)
	  {
		  CloseHandle(g_db_hFile);
		  mystrcpy(m_data_file_path, state_exe_path);
		  psz =_tcsrchr(m_data_file_path,'\\');
		  if (psz==NULL) psz=m_data_file_path;
		  while (psz2=_tcschr(psz+1,'.'))
		  {
			  psz = psz2;
		  }
		  wsprintf(psz,_T(".%u.dat"),cur_index);
		  g_db_hFile = db_hFile = myOpenFile(m_data_file_path, GENERIC_READ, OPEN_EXISTING);
		  if (db_hFile == INVALID_HANDLE_VALUE)
		  {
			  return _LANG_CANTOPENSELF;
		  }
		  if (!ReadSelfFile(&dh, sizeof(dh)))
		  {// read out the data file header
			  return _LANG_INVALIDCRC;
		  }	
		  if (cur_index==1)
		  {
			 m_length = dh.total_length;
		  }
		  if (!GetFileSizeEx(db_hFile,(LARGE_INTEGER*)&length) || length!=dh.length+sizeof(dataheader) || dh.volume_index!=cur_index)
		  {// check the whole file length
			  return _LANG_INVALIDCRC;
		  }
		  if (m_file_mapping.first==NULL)
		  {// it's the first time to meet a data file
			  m_file_mapping.cur = GlobalAlloc(GPTR,sizeof(struct FileMapping));
			  m_file_mapping.first = m_file_mapping.cur;
		  }
		  else
		  {// push the new data file to the end, and replace the current one
			  m_file_mapping.cur->next = GlobalAlloc(GPTR,sizeof(struct FileMapping));
			  m_file_mapping.cur = m_file_mapping.cur->next;
		  }
		  mini_memcpy(&m_file_mapping.cur->dh,&dh,sizeof(dh));
		  m_file_mapping.cur->next = NULL;
#ifdef NSIS_CONFIG_CRC_SUPPORT
		  if (do_crc)
		  {
			  __int64 cur_length= 0;
			  crc = 0;
			  while (cur_length<dh.length)
			  {
				  static char temp[32768*32]; // modified by yew: for fast crc
				  DWORD l = min(dh.length-cur_length, sizeof(temp));
				  if (!ReadSelfFile(temp, l))
				  {
#if defined(NSIS_CONFIG_VISIBLE_SUPPORT)
					  handle_ver_dlg(TRUE);
#endif
					  return _LANG_INVALIDCRC;
				  }
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT

#ifdef NSIS_CONFIG_SILENT_SUPPORT
				  if ((cl_flags & FH_FLAGS_SILENT) == 0)
#endif//NSIS_CONFIG_SILENT_SUPPORT
				  {
					  handle_ver_dlg(FALSE);
				  }
#endif//NSIS_CONFIG_VISIBLE_SUPPORT

				  crc = CRC32(crc, (unsigned char*)temp, l);

				  m_pos += l;
				  cur_length += l;
			  }
			  if (dh.crc != crc)
				  return _LANG_INVALIDCRC;
		  }
#endif//NSIS_CONFIG_CRC_SUPPORT
		  if (cur_index<dh.total_volume)
			  cur_index++;// need to check next file
		  else
			  break;
	  }
#ifdef NSIS_CONFIG_CRC_SUPPORT
	  if (do_crc)
	  {
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
		  handle_ver_dlg(TRUE);
#endif//NSIS_CONFIG_VISIBLE_SUPPORT
	  }
#endif//NSIS_CONFIG_CRC_SUPPORT
	  // re-open the first file
	  if (cur_index!=1)
	  {
		  CloseHandle(g_db_hFile);
		  mystrcpy(m_data_file_path, state_exe_path);
		  psz =_tcsrchr(m_data_file_path,'\\');
		  if (psz==NULL) psz=m_data_file_path;
		  while (psz2=_tcschr(psz+1,'.'))
		  {
			  psz = psz2;
		  }
		  wsprintf(psz,_T(".%u.dat"),1);
		  g_db_hFile = db_hFile = myOpenFile(m_data_file_path, GENERIC_READ, OPEN_EXISTING);
	  }
	  m_file_mapping.cur = m_file_mapping.first;// reset the current mapping to the first one
	  m_file_mapping.LoadFinished = TRUE;// if there is no data file, keep it to be FALSE always
	  header->blocks[NB_DATA].offset = SetFilePointer(db_hFile,0,NULL,FILE_BEGIN);
	  m_file_mapping.cur_offset = 0;
	}
	header->blocks[NB_DATA].offset = SetFilePointer(db_hFile,0,NULL,FILE_CURRENT);
#endif

  mini_memcpy(&g_blocks, &header->blocks, sizeof(g_blocks));

  return 0;
}

#define IBUFSIZE (16384*32) // modified by yew: for fast crc
#define OBUFSIZE (32768*32)

// returns -3 if compression error/eof/etc

#if !defined(NSIS_COMPRESS_WHOLE) || !defined(NSIS_CONFIG_COMPRESSION_SUPPORT)

// Decompress data.
__int64 NSISCALL _dodecomp(__int64 offset, HANDLE hFileOut, unsigned char *outbuf, int outbuflen)
{
  static char inbuffer[IBUFSIZE+OBUFSIZE];
  char *outbuffer;
  __int64 outbuffer_len=outbuf?outbuflen:OBUFSIZE;
  __int64 retval=0;
  __int64 input_len;

  outbuffer = outbuf?(char*)outbuf:(inbuffer+IBUFSIZE);

  if (offset>=0)
  {
    SetSelfFilePointer(g_blocks[NB_DATA].offset+offset);
  }

  if (!ReadSelfFile((LPVOID)&input_len,sizeof(__int64))) return -3;

#ifdef NSIS_CONFIG_COMPRESSION_SUPPORT
  if (input_len & COMPRESSED_FLAG_MARK /* 0x80000000*/) // compressed , modified by yew
  {
    TCHAR progress[64];
    __int64 input_len_total;
    DWORD ltc = GetTickCount(), tc;

    inflateReset(&g_inflate_stream);
    input_len_total = input_len &= /*0x7fffffff*/~COMPRESSED_FLAG_MARK; // take off top bit.

    while (input_len > 0)
    {
      __int64 l=min(input_len,(__int64)IBUFSIZE);
      int err;

      if (!ReadSelfFile((LPVOID)inbuffer,l))
        return -3;

      g_inflate_stream.next_in = inbuffer;
      g_inflate_stream.avail_in = l;
      input_len-=l;

      for (;;)
      {
        int u;

        g_inflate_stream.next_out = outbuffer;
        g_inflate_stream.avail_out = (unsigned int)outbuffer_len;

        err=inflate(&g_inflate_stream);

        if (err<0) return -4;

        u=(char*)g_inflate_stream.next_out - outbuffer;

        tc = GetTickCount();
        if (g_exec_flags.status_update & 1 && (tc - ltc > 200 || !input_len))
        {
          wsprintf(progress, _T("... %d%%"), (input_len_total - input_len) * 100/input_len_total);
          update_status_text(0, progress);
          ltc = tc;
        }

        // if there's no output, more input is needed
        if (!u)
          break;

        if (!outbuf)
        {
          DWORD r;
          if (!WriteFile(hFileOut,outbuffer,u,&r,NULL) || (int)r != u) return -2;
          retval+=u;
        }
        else
        {
          retval+=u;
          outbuffer_len-=u;
          outbuffer=g_inflate_stream.next_out;
        }
        if (err==Z_STREAM_END) return retval;
      }
    }
  }
  else
#endif//NSIS_CONFIG_COMPRESSION_SUPPORT
  {
    if (!outbuf)
    {
      while (input_len > 0)
      {
        DWORD l=min(input_len,outbuffer_len);
        DWORD t;
        if (!ReadSelfFile((LPVOID)inbuffer,l)) return -3;
        if (!WriteFile(hFileOut,inbuffer,l,&t,NULL) || l!=t) return -2;
        retval+=l;
        input_len-=l;
      }
    }
    else
    {
      int l=min(input_len,outbuflen);
      if (!ReadSelfFile((LPVOID)outbuf,l)) return -3;
      retval=l;
    }
  }
  return retval;
}
#else//NSIS_COMPRESS_WHOLE

static char _inbuffer[IBUFSIZE];
static char _outbuffer[OBUFSIZE];
extern __int64 m_length;
extern __int64 m_pos;
extern BOOL CALLBACK verProc(HWND, UINT, WPARAM, LPARAM);
extern BOOL CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
static int NSISCALL __ensuredata(int amount)
{
  int needed=amount-(dbd_size-dbd_pos);
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
  verify_time=GetTickCount()+500;
#endif
  if (needed>0)
  {
    SetSelfFilePointer(dbd_srcpos);
    SetFilePointerEx(dbd_hFile,(LARGE_INTEGER*)&dbd_size,NULL,FILE_BEGIN);
    m_length=needed;
    m_pos=0;
    for (;;)
    {
      int err;
      int l=min(IBUFSIZE,dbd_fulllen-dbd_srcpos);
      if (!ReadSelfFile((LPVOID)_inbuffer,l)) return -1;
      dbd_srcpos+=l;
      g_inflate_stream.next_in=_inbuffer;
      g_inflate_stream.avail_in=l;
      do
      {
        DWORD r,t;
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
        if (g_header)
#ifdef NSIS_CONFIG_SILENT_SUPPORT
          if (!g_exec_flags.silent)
#endif
          {
            m_pos=m_length-(amount-(dbd_size-dbd_pos));

            handle_ver_dlg(FALSE);
          }
#endif//NSIS_CONFIG_VISIBLE_SUPPORT
        g_inflate_stream.next_out=_outbuffer;
        g_inflate_stream.avail_out=OBUFSIZE;
        err=inflate(&g_inflate_stream);
        if (err<0)
        {
          return -3;
        }
        r=(DWORD)g_inflate_stream.next_out-(DWORD)_outbuffer;
        if (r)
        {
          if (!WriteFile(dbd_hFile,_outbuffer,r,&t,NULL) || r != t)
          {
            return -2;
          }
          dbd_size+=r;
        }
        else if (g_inflate_stream.avail_in || !l) return -3;
        else break;
      }
      while (g_inflate_stream.avail_in);
      if (amount-(dbd_size-dbd_pos) <= 0) break;
    }
    SetFilePointerEx(dbd_hFile,(LARGE_INTEGER*)&dbd_pos,NULL,FILE_BEGIN);
  }
#ifdef NSIS_CONFIG_VISIBLE_SUPPORT
  handle_ver_dlg(TRUE);
#endif//NSIS_CONFIG_VISIBLE_SUPPORT
  return 0;
}


__int64 NSISCALL _dodecomp(__int64 offset, HANDLE hFileOut, unsigned char *outbuf, int outbuflen)
{
  DWORD r;
  __int64 input_len;
  __int64 retval;
  if (offset>=0)
  {
    dbd_pos=g_blocks[NB_DATA].offset+offset;
    SetFilePointerEx(dbd_hFile,(LARGE_INTEGER*)&dbd_pos,NULL,FILE_BEGIN);
  }
  retval=__ensuredata(sizeof(__int64));
  if (retval<0) return retval;

  if (!ReadFile(dbd_hFile,(LPVOID)&input_len,sizeof(__int64),&r,NULL) || r!=sizeof(__int64)) return -3;
  dbd_pos+=sizeof(__int64);

  retval=__ensuredata(input_len);
  if (retval < 0) return retval;

  if (!outbuf)
  {
    while (input_len > 0)
    {
      DWORD t;
      DWORD l=min(input_len,IBUFSIZE);
      if (!ReadFile(dbd_hFile,(LPVOID)_inbuffer,l,&r,NULL) || l != r) return -3;
      if (!WriteFile(hFileOut,_inbuffer,r,&t,NULL) || t != l) return -2;
      retval+=r;
      input_len-=r;
      dbd_pos+=r;
    }
  }
  else
  {
    if (!ReadFile(dbd_hFile,(LPVOID)outbuf,min(input_len,outbuflen),&r,NULL)) return -3;
    retval=r;
    dbd_pos+=r;
  }
  return retval;
}
#endif//NSIS_COMPRESS_WHOLE

BOOL NSISCALL ReadSelfFile(LPVOID lpBuffer, DWORD nNumberOfBytesToRead)
{
  DWORD rd,i;
  if (!m_file_mapping.LoadFinished)
  {
	  return ReadFile(g_db_hFile,lpBuffer,nNumberOfBytesToRead,&rd,NULL) && (rd == nNumberOfBytesToRead);
  }
  for (i=0;i<nNumberOfBytesToRead;)
  {
	  __int64 offset=m_file_mapping.cur_offset-(m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume;
	  if (offset < m_file_mapping.cur->dh.length)
	  {
		  // there is still some data,read them first
		  DWORD l= min(m_file_mapping.cur->dh.length-offset,(__int64)nNumberOfBytesToRead-i);
		  if (!ReadFile(g_db_hFile,lpBuffer,l,&rd,NULL) || (rd != l))
			  return FALSE;

		  i += l;
		  m_file_mapping.cur_offset += l;
		  lpBuffer = (char*)lpBuffer + l;
	  }
	  else
	  {
		  LPTSTR psz,psz2;
		  assert(m_file_mapping.cur->next);
		  m_file_mapping.cur = m_file_mapping.cur->next;
		  CloseHandle(g_db_hFile);
		  mystrcpy(m_data_file_path, state_exe_path);
		  psz =_tcsrchr(m_data_file_path,'\\');
		  if (psz==NULL) psz=m_data_file_path;
		  while (psz2=_tcschr(psz+1,'.'))
		  {
			  psz = psz2;
		  }
		  wsprintf(psz,_T(".%u.dat"),m_file_mapping.cur->dh.volume_index);
		  g_db_hFile = myOpenFile(m_data_file_path, GENERIC_READ, OPEN_EXISTING);
		  SetFilePointer(g_db_hFile,sizeof(dataheader),NULL,FILE_BEGIN);
	  }
  }
  return TRUE;
}

__int64 NSISCALL SetSelfFilePointer(__int64 lDistanceToMove)
{
	__int64 ret;
	if (!m_file_mapping.LoadFinished)
	{
	  SetFilePointerEx(g_db_hFile,*(LARGE_INTEGER*)&lDistanceToMove,(LARGE_INTEGER*)&ret,FILE_BEGIN);
	  return ret;
	}
	// need to check the valid range
	if (lDistanceToMove>=(m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume && 
		lDistanceToMove<(m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume+m_file_mapping.cur->dh.length)
	{
		// it's okay, locate in the current file
		m_file_mapping.cur_offset = lDistanceToMove;
		lDistanceToMove -= (m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume; // convert to the local offset
		lDistanceToMove += sizeof(dataheader);
		SetFilePointerEx(g_db_hFile,*(LARGE_INTEGER*)&lDistanceToMove,(LARGE_INTEGER*)&ret,FILE_BEGIN);
		return /*m_file_mapping.cur_offset*/0;// the return is ignored
	}

	// open the correct file
	m_file_mapping.cur = m_file_mapping.first;
	while(m_file_mapping.cur)
	{
		if (lDistanceToMove>=(m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume && 
			lDistanceToMove<(m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume+m_file_mapping.cur->dh.length)
		{
			// found it
			LPTSTR psz,psz2;
			CloseHandle(g_db_hFile);
			mystrcpy(m_data_file_path, state_exe_path);
			psz =_tcsrchr(m_data_file_path,'\\');
			if (psz==NULL) psz=m_data_file_path;
			while (psz2=_tcschr(psz+1,'.'))
			{
				psz = psz2;
			}
			wsprintf(psz,_T(".%u.dat"),m_file_mapping.cur->dh.volume_index);
			g_db_hFile = myOpenFile(m_data_file_path, GENERIC_READ, OPEN_EXISTING);

			m_file_mapping.cur_offset = lDistanceToMove;
			lDistanceToMove -= (m_file_mapping.cur->dh.volume_index-1)*m_file_mapping.cur->dh.length_per_volume; // convert to the local offset
			lDistanceToMove += sizeof(dataheader);
			SetFilePointerEx(g_db_hFile,*(LARGE_INTEGER*)&lDistanceToMove,(LARGE_INTEGER*)&ret,FILE_BEGIN);
			return /*m_file_mapping.cur_offset*/0;//the return is ignored
		}
		m_file_mapping.cur = m_file_mapping.cur->next;
	}
	assert(!"shouldn't happen");
	return 0;
}
