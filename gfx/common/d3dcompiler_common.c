/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2018 - Ali Bouhlel
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#define CINTERFACE

#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include <encodings/crc32.h>
#include <features/features_cpu.h>

#include "d3dcompiler_common.h"
#include "dxgi_common.h"
#include "../video_shader_parse.h"
#include "../../verbosity.h"

#if defined(HAVE_DYLIB) && !defined(__WINRT__)
#include <dynamic/dylib.h>

/* TODO/FIXME - static globals */
static dylib_t     d3dcompiler_dll;
static const char* d3dcompiler_dll_list[] = {
   "D3DCompiler_47.dll", "D3DCompiler_46.dll", "D3DCompiler_45.dll", "D3DCompiler_44.dll",
   "D3DCompiler_43.dll", "D3DCompiler_42.dll", "D3DCompiler_41.dll", "D3DCompiler_40.dll",
   "D3DCompiler_39.dll", "D3DCompiler_38.dll", "D3DCompiler_37.dll", "D3DCompiler_36.dll",
   "D3DCompiler_35.dll", "D3DCompiler_34.dll", "D3DCompiler_33.dll", NULL,
};

static pD3DCompile d3dcompile_fp;
typedef HRESULT(WINAPI * pD3DCreateBlob)(SIZE_T, ID3DBlob**);
static pD3DCreateBlob d3dcreateblob_fp;
/* The DLL that compiled, part of every cache key: another compiler
 * version may emit other bytecode for the same source. */
static const char* d3dcompiler_dll_name = "";

/* Resolves D3DCompile once.  The deferred shader loads compile on the
 * task threads, several at a time, so they call this from the thread
 * that starts the load first and only ever read what it wrote. */
static bool d3d_compiler_resolve(void)
{
   const char** dll_name = d3dcompiler_dll_list;
   if (d3dcompile_fp)
      return true;
   while (!d3dcompiler_dll && *dll_name)
   {
      d3dcompiler_dll_name = *dll_name;
      d3dcompiler_dll      = dylib_load(*dll_name++);
   }
   if (!d3dcompiler_dll)
      return false;
   d3dcreateblob_fp = (pD3DCreateBlob)dylib_proc(d3dcompiler_dll,
         "D3DCreateBlob");
   d3dcompile_fp = (pD3DCompile)dylib_proc(d3dcompiler_dll, "D3DCompile");
   return d3dcompile_fp != NULL;
}

HRESULT WINAPI D3DCompile(
      LPCVOID pSrcData,
      SIZE_T  SrcDataSize,
      LPCSTR  pSourceName,
      CONST D3D_SHADER_MACRO* pDefines,
      ID3DInclude*            pInclude,
      LPCSTR                  pEntrypoint,
      LPCSTR                  pTarget,
      UINT                    Flags1,
      UINT                    Flags2,
      ID3DBlob**              ppCode,
      ID3DBlob**              ppErrorMsgs)
{
   if (!d3d_compiler_resolve())
      return d3dcompiler_dll ? TYPE_E_DLLFUNCTIONNOTFOUND
         : TYPE_E_CANTLOADLIBRARY;
   return d3dcompile_fp(
         pSrcData, SrcDataSize, pSourceName,
         pDefines, pInclude, pEntrypoint, pTarget,
         Flags1,   Flags2, ppCode, ppErrorMsgs);
}

HRESULT WINAPI D3DCompileFromFile(
      LPCWSTR                 pFileName,
      const D3D_SHADER_MACRO* pDefines,
      ID3DInclude*            pInclude,
      LPCSTR                  pEntrypoint,
      LPCSTR                  pTarget,
      UINT                    Flags1,
      UINT                    Flags2,
      ID3DBlob**              ppCode,
      ID3DBlob**              ppErrorMsgs)
{
   typedef HRESULT(WINAPI * pD3DCompileFromFile)(
         LPCWSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
         ID3DBlob**);
   static pD3DCompileFromFile fp;
   const char** dll_name = d3dcompiler_dll_list;
   while (!d3dcompiler_dll && *dll_name)
      d3dcompiler_dll = dylib_load(*dll_name++);

   if (!d3dcompiler_dll)
	   return TYPE_E_CANTLOADLIBRARY;

   if (!fp)
      if (!(fp = (pD3DCompileFromFile)dylib_proc(d3dcompiler_dll,
"D3DCompileFromFile")))
         return TYPE_E_DLLFUNCTIONNOTFOUND;
   return fp(
         pFileName, pDefines, pInclude, pEntrypoint, pTarget, Flags1, Flags2, ppCode, ppErrorMsgs);
}

HRESULT WINAPI
        D3DReflect(LPCVOID pSrcData, SIZE_T SrcDataSize, REFIID pInterface, void** ppReflector)
{
   typedef HRESULT(WINAPI * pD3DCompileFromFile)(LPCVOID, SIZE_T, REFIID, void**);
   static pD3DCompileFromFile fp;

   const char** dll_name = d3dcompiler_dll_list;
   while (!d3dcompiler_dll && *dll_name)
      d3dcompiler_dll = dylib_load(*dll_name++);

   if (!d3dcompiler_dll)
      return TYPE_E_CANTLOADLIBRARY;

   if (!fp)
      if (!(fp = (pD3DCompileFromFile)dylib_proc(d3dcompiler_dll,
"D3DReflect")))
         return TYPE_E_DLLFUNCTIONNOTFOUND;
   return fp(pSrcData, SrcDataSize, pInterface, ppReflector);
}
#endif

bool d3d_compiler_load(void)
{
#if defined(HAVE_DYLIB) && !defined(__WINRT__)
   return d3d_compiler_resolve();
#else
   return true;
#endif
}

/* Names the bytecode for one compile: source, entry point, target,
 * flags and compiler.  Two CRC32 sums of the source (the second seeded
 * by everything else) and its length are 128 bits, the width the
 * SPIR-V cache uses, so unrelated shaders do not share an entry. */
static bool d3d_compile_cache_key(const char* src, size_t len,
      LPCSTR entrypoint, LPCSTR target, UINT flags, char* key, size_t key_len)
{
   char     meta[160];
   uint32_t meta_sum;
   int      meta_len;
#if defined(HAVE_DYLIB) && !defined(__WINRT__)
   const char *compiler = d3dcompiler_dll_name;
#else
   const char *compiler = "linked";
#endif

   if (len > 0xffffffffu)
      return false;
   meta_len = snprintf(meta, sizeof(meta), "%s|%s|%u|%s",
         entrypoint ? entrypoint : "", target ? target : "",
         (unsigned)flags, compiler);
   if (meta_len < 0 || (size_t)meta_len >= sizeof(meta))
      return false;
   meta_sum = encoding_crc32(0, (const uint8_t*)meta, (size_t)meta_len);
   return (size_t)snprintf(key, key_len, "%08x%08x%08x%08x",
         (unsigned)encoding_crc32(0, (const uint8_t*)src, len),
         (unsigned)encoding_crc32(meta_sum, (const uint8_t*)src, len),
         (unsigned)len,
         (unsigned)meta_sum) < key_len;
}

/* A DXBC container opens with "DXBC", a 16-byte checksum and a version,
 * then its total size.  An entry is used only when that size is exactly
 * the bytes read, so a torn or foreign file is compiled over instead. */
static bool d3d_compile_cache_entry_ok(const uint8_t* data, int64_t len)
{
   uint32_t total;
   if (len < 32 || memcmp(data, "DXBC", 4))
      return false;
   total = (uint32_t)data[24]         | ((uint32_t)data[25] << 8)
         | ((uint32_t)data[26] << 16) | ((uint32_t)data[27] << 24);
   return (int64_t)total == len;
}

static bool d3d_compile_cache_load(const char* key, D3DBlob* out)
{
   video_shader_cache_view_t view;
   D3DBlob                   blob = NULL;
   bool                      ok   = false;
#if defined(HAVE_DYLIB) && !defined(__WINRT__)
   if (!d3dcreateblob_fp)
      return false;
#endif
   if (!video_shader_cache_map(VIDEO_SHADER_CACHE_D3D_BLOB, key, &view))
      return false;
   if (d3d_compile_cache_entry_ok(view.data, view.len))
   {
#if defined(HAVE_DYLIB) && !defined(__WINRT__)
      HRESULT hr = d3dcreateblob_fp((SIZE_T)view.len, &blob);
#else
      HRESULT hr = D3DCreateBlob((SIZE_T)view.len, &blob);
#endif
      if (SUCCEEDED(hr) && blob)
      {
         memcpy(blob->lpVtbl->GetBufferPointer(blob), view.data,
               (size_t)view.len);
         *out = blob;
         ok   = true;
      }
   }
   video_shader_cache_unmap(&view);
   return ok;
}

bool d3d_compile(const char* src, size_t len,
      LPCSTR src_name, LPCSTR entrypoint, LPCSTR target, D3DBlob* out)
{
   D3DBlob err = NULL;
   char    key[40];
   bool    keyed;
   retro_time_t start;
#ifdef DEBUG
   UINT compileflags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
   UINT compileflags = 0;
#endif

   if (!len)
      len = strlen(src);

#if defined(HAVE_DYLIB) && !defined(__WINRT__)
   /* The key names the DLL, so it has to be loaded first */
   d3d_compiler_resolve();
#endif
   keyed = d3d_compile_cache_key(src, len, entrypoint, target,
         compileflags, key, sizeof(key));
   if (keyed && d3d_compile_cache_load(key, out))
   {
      RARCH_LOG("[D3DCompile] %s %s: from cache.\n",
            src_name ? src_name : "", target ? target : "");
      return true;
   }

   start = cpu_features_get_time_usec();
   if (FAILED(D3DCompile(
             src, len, src_name, NULL, NULL,
             entrypoint, target, compileflags, 0, out, &err)))
   {
      if (err)
      {
         const char* msg = (const char*)err->lpVtbl->GetBufferPointer(err);
         if (msg)
         {
            RARCH_ERR("D3DCompile failed: %s.\n", msg);
            /* Place a breakpoint here, if you want,
               to see shader compilation issues */
         }
         Release(err);
      }
      return false;
   }

   RARCH_LOG("[D3DCompile] %s %s: compiled in %u ms.\n",
         src_name ? src_name : "", target ? target : "",
         (unsigned)((cpu_features_get_time_usec() - start) / 1000));
   if (keyed && *out)
      video_shader_cache_write(VIDEO_SHADER_CACHE_D3D_BLOB, key,
            (*out)->lpVtbl->GetBufferPointer(*out),
            (size_t)(*out)->lpVtbl->GetBufferSize(*out));

   return true;
}

bool d3d_compile_from_file(LPCWSTR filename,
      LPCSTR entrypoint, LPCSTR target, D3DBlob* out)
{
   D3DBlob err = NULL;
#ifdef DEBUG
   UINT compileflags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
   UINT compileflags = 0;
#endif

   if (FAILED(D3DCompileFromFile(
             filename, NULL, NULL, entrypoint,
             target, compileflags, 0, out, &err)))
   {
      if (err)
      {
         const char* msg = (const char*)err->lpVtbl->GetBufferPointer(err);
         if (msg)
            RARCH_ERR("D3DCompile failed: %s.\n", msg);
         Release(err);
      }
      return false;
   }

   return true;
}
