/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_META_H_
#define _TD_META_H_

#include "mallocator.h"
#include "os.h"
#include "tmsg.h"
#include "trow.h"

#ifdef __cplusplus
extern "C" {
#endif

// Types exported
typedef struct SMeta SMeta;

#define META_SUPER_TABLE 0
#define META_CHILD_TABLE 1
#define META_NORMAL_TABLE 2

typedef struct SMetaCfg {
  /// LRU cache size
  uint64_t lruSize;
} SMetaCfg;

typedef struct {
  int32_t  nCols;
  SSchema *pSchema;
} SSchemaWrapper;

typedef SVCreateTbReq STbCfg;

// SMeta operations
SMeta *metaOpen(const char *path, const SMetaCfg *pMetaCfg, SMemAllocatorFactory *pMAF);
void   metaClose(SMeta *pMeta);
void   metaRemove(const char *path);
int    metaCreateTable(SMeta *pMeta, STbCfg *pTbCfg);
int    metaDropTable(SMeta *pMeta, tb_uid_t uid);
int    metaCommit(SMeta *pMeta);

// For Query
STbCfg *        metaGetTbInfoByUid(SMeta *pMeta, tb_uid_t uid);
STbCfg *        metaGetTbInfoByName(SMeta *pMeta, char *tbname, tb_uid_t *uid);
SSchemaWrapper *metaGetTableSchema(SMeta *pMeta, tb_uid_t uid, int32_t sver, bool isinline);

// Options
void metaOptionsInit(SMetaCfg *pMetaCfg);
void metaOptionsClear(SMetaCfg *pMetaCfg);

#ifdef __cplusplus
}
#endif

#endif /*_TD_META_H_*/
