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
#include "tdbInt.h"

// #include <sys/types.h>
// #include <unistd.h>

struct SPCache {
  int         szPage;
  int         nPages;
  SPage     **aPage;
  tdb_mutex_t mutex;
  int         nFree;
  SPage      *pFree;
  int         nPage;
  int         nHash;
  SPage     **pgHash;
  int         nRecyclable;
  SPage       lru;
};

static inline uint32_t tdbPCachePageHash(const SPgid *pPgid) {
  uint32_t *t = (uint32_t *)((pPgid)->fileid);
  return (uint32_t)(t[0] + t[1] + t[2] + t[3] + t[4] + t[5] + (pPgid)->pgno);
}

static int    tdbPCacheOpenImpl(SPCache *pCache);
static SPage *tdbPCacheFetchImpl(SPCache *pCache, const SPgid *pPgid, TXN *pTxn);
static void   tdbPCachePinPage(SPCache *pCache, SPage *pPage);
static void   tdbPCacheRemovePageFromHash(SPCache *pCache, SPage *pPage);
static void   tdbPCacheAddPageToHash(SPCache *pCache, SPage *pPage);
static void   tdbPCacheUnpinPage(SPCache *pCache, SPage *pPage);
static int    tdbPCacheCloseImpl(SPCache *pCache);

static void tdbPCacheInitLock(SPCache *pCache) { tdbMutexInit(&(pCache->mutex), NULL); }
static void tdbPCacheDestroyLock(SPCache *pCache) { tdbMutexDestroy(&(pCache->mutex)); }
static void tdbPCacheLock(SPCache *pCache) { tdbMutexLock(&(pCache->mutex)); }
static void tdbPCacheUnlock(SPCache *pCache) { tdbMutexUnlock(&(pCache->mutex)); }

int tdbPCacheOpen(int pageSize, int cacheSize, SPCache **ppCache) {
  SPCache *pCache;
  void    *pPtr;
  SPage   *pPgHdr;

  pCache = (SPCache *)tdbOsCalloc(1, sizeof(*pCache) + sizeof(SPage *) * cacheSize);
  if (pCache == NULL) {
    return -1;
  }

  pCache->szPage = pageSize;
  pCache->nPages = cacheSize;
  pCache->aPage = (SPage **)tdbOsCalloc(cacheSize, sizeof(SPage *));
  if (pCache->aPage == NULL) {
    tdbOsFree(pCache);
    return -1;
  }

  if (tdbPCacheOpenImpl(pCache) < 0) {
    tdbOsFree(pCache);
    return -1;
  }

  *ppCache = pCache;
  return 0;
}

int tdbPCacheClose(SPCache *pCache) {
  if (pCache) {
    tdbPCacheCloseImpl(pCache);
    tdbOsFree(pCache->aPage);
    tdbOsFree(pCache);
  }
  return 0;
}

// TODO:
// if (pPage->id >= pCache->nPages) {
//   free(pPage);
//   pCache->aPage[pPage->id] = NULL;
// } else {
//   add to free list
// }

static int tdbPCacheAlterImpl(SPCache *pCache, int32_t nPage) {
  if (pCache->nPages == nPage) {
    return 0;
  } else if (pCache->nPages < nPage) {
    SPage **aPage = tdbOsCalloc(nPage, sizeof(SPage *));
    if (aPage == NULL) {
      return -1;
    }

    for (int32_t iPage = pCache->nPages; iPage < nPage; iPage++) {
      if (tdbPageCreate(pCache->szPage, &aPage[iPage], tdbDefaultMalloc, NULL) < 0) {
        // TODO: handle error
        tdbOsFree(aPage);
        return -1;
      }

      // pPage->pgid = 0;
      aPage[iPage]->isAnchor = 0;
      aPage[iPage]->isLocal = 1;
      aPage[iPage]->nRef = 0;
      aPage[iPage]->pHashNext = NULL;
      aPage[iPage]->pLruNext = NULL;
      aPage[iPage]->pLruPrev = NULL;
      aPage[iPage]->pDirtyNext = NULL;

      // add to local list
      aPage[iPage]->id = iPage;
    }

    // add page to free list
    for (int32_t iPage = pCache->nPages; iPage < nPage; iPage++) {
      aPage[iPage]->pFreeNext = pCache->pFree;
      pCache->pFree = aPage[iPage];
      pCache->nFree++;
    }

    for (int32_t iPage = 0; iPage < pCache->nPage; iPage++) {
      aPage[iPage] = pCache->aPage[iPage];
    }

    tdbOsFree(pCache->aPage);
    pCache->aPage = aPage;
  } else {
    for (SPage **ppPage = &pCache->pFree; *ppPage;) {
      int32_t iPage = (*ppPage)->id;

      if (iPage >= nPage) {
        SPage *pPage = *ppPage;
        *ppPage = pPage->pFreeNext;
        pCache->aPage[pPage->id] = NULL;
        tdbPageDestroy(pPage, tdbDefaultFree, NULL);
        pCache->nFree--;
      } else {
        ppPage = &(*ppPage)->pFreeNext;
      }
    }
  }

  pCache->nPages = nPage;
  return 0;
}

int tdbPCacheAlter(SPCache *pCache, int32_t nPage) {
  int ret = 0;

  tdbPCacheLock(pCache);

  ret = tdbPCacheAlterImpl(pCache, nPage);

  tdbPCacheUnlock(pCache);

  return ret;
}

SPage *tdbPCacheFetch(SPCache *pCache, const SPgid *pPgid, TXN *pTxn) {
  SPage *pPage;
  i32    nRef;

  tdbPCacheLock(pCache);

  pPage = tdbPCacheFetchImpl(pCache, pPgid, pTxn);
  if (pPage) {
    nRef = tdbRefPage(pPage);
  }

  ASSERT(pPage);

  tdbPCacheUnlock(pCache);

  // printf("thread %" PRId64 " fetch page %d pgno %d pPage %p nRef %d\n", taosGetSelfPthreadId(), pPage->id,
  //        TDB_PAGE_PGNO(pPage), pPage, nRef);

  tdbDebug("pcache/fetch page %p/%d/%d/%d", pPage, TDB_PAGE_PGNO(pPage), pPage->id, nRef);
  return pPage;
}

void tdbPCacheRelease(SPCache *pCache, SPage *pPage, TXN *pTxn) {
  i32 nRef;

  ASSERT(pTxn);

  // nRef = tdbUnrefPage(pPage);
  // ASSERT(nRef >= 0);

  tdbPCacheLock(pCache);
  nRef = tdbUnrefPage(pPage);
  tdbDebug("pcache/release page %p/%d/%d/%d", pPage, TDB_PAGE_PGNO(pPage), pPage->id, nRef);
  if (nRef == 0) {
    // test the nRef again to make sure
    // it is safe th handle the page
    // nRef = tdbGetPageRef(pPage);
    // if (nRef == 0) {
    if (pPage->isLocal) {
      tdbPCacheUnpinPage(pCache, pPage);
    } else {
      if (TDB_TXN_IS_WRITE(pTxn)) {
        // remove from hash
        tdbPCacheRemovePageFromHash(pCache, pPage);
      }

      tdbPageDestroy(pPage, pTxn->xFree, pTxn->xArg);
    }
    // }
  }
  tdbPCacheUnlock(pCache);
  // printf("thread %" PRId64 " relas page %d pgno %d pPage %p nRef %d\n", taosGetSelfPthreadId(), pPage->id,
  //        TDB_PAGE_PGNO(pPage), pPage, nRef);
}

int tdbPCacheGetPageSize(SPCache *pCache) { return pCache->szPage; }

static SPage *tdbPCacheFetchImpl(SPCache *pCache, const SPgid *pPgid, TXN *pTxn) {
  int    ret = 0;
  SPage *pPage = NULL;
  SPage *pPageH = NULL;

  ASSERT(pTxn);

  // 1. Search the hash table
  pPage = pCache->pgHash[tdbPCachePageHash(pPgid) % pCache->nHash];
  while (pPage) {
    if (pPage->pgid.pgno == pPgid->pgno && memcmp(pPage->pgid.fileid, pPgid->fileid, TDB_FILE_ID_LEN) == 0) break;
    pPage = pPage->pHashNext;
  }

  if (pPage) {
    if (pPage->isLocal || TDB_TXN_IS_WRITE(pTxn)) {
      tdbPCachePinPage(pCache, pPage);
      return pPage;
    }
  }

  // 1. pPage == NULL
  // 2. pPage && pPage->isLocal == 0 && !TDB_TXN_IS_WRITE(pTxn)
  pPageH = pPage;
  pPage = NULL;

  // 2. Try to allocate a new page from the free list
  if (pCache->pFree) {
    pPage = pCache->pFree;
    pCache->pFree = pPage->pFreeNext;
    pCache->nFree--;
    pPage->pLruNext = NULL;
  }

  // 3. Try to Recycle a page
  if (!pPage && !pCache->lru.pLruPrev->isAnchor) {
    pPage = pCache->lru.pLruPrev;
    tdbPCacheRemovePageFromHash(pCache, pPage);
    tdbPCachePinPage(pCache, pPage);
  }

  // 4. Try a create new page
  if (!pPage) {
    ret = tdbPageCreate(pCache->szPage, &pPage, pTxn->xMalloc, pTxn->xArg);
    if (ret < 0 || pPage == NULL) {
      // TODO
      ASSERT(0);
      return NULL;
    }

    // init the page fields
    pPage->isAnchor = 0;
    pPage->isLocal = 0;
    pPage->nRef = 0;
    pPage->id = -1;
  }

  // 5. Page here are just created from a free list
  // or by recycling or allocated streesly,
  // need to initialize it
  if (pPage) {
    if (pPageH) {
      // copy the page content
      memcpy(&(pPage->pgid), pPgid, sizeof(*pPgid));

      for (int nLoops = 0;;) {
        if (pPageH->pPager) break;
        if (++nLoops > 1000) {
          sched_yield();
          nLoops = 0;
        }
      }

      pPage->pLruNext = NULL;
      pPage->pPager = pPageH->pPager;

      memcpy(pPage->pData, pPageH->pData, pPage->pageSize);
      // tdbDebug("pcache/pPageH: %p %ld %p %p %u", pPageH, pPageH->pPageHdr - pPageH->pData, pPageH->xCellSize, pPage,
      //         TDB_PAGE_PGNO(pPageH));
      tdbPageInit(pPage, pPageH->pPageHdr - pPageH->pData, pPageH->xCellSize);
      pPage->kLen = pPageH->kLen;
      pPage->vLen = pPageH->vLen;
      pPage->maxLocal = pPageH->maxLocal;
      pPage->minLocal = pPageH->minLocal;
    } else {
      memcpy(&(pPage->pgid), pPgid, sizeof(*pPgid));
      pPage->pLruNext = NULL;
      pPage->pPager = NULL;

      if (pPage->isLocal || TDB_TXN_IS_WRITE(pTxn)) {
        tdbPCacheAddPageToHash(pCache, pPage);
      }
    }
  }

  return pPage;
}

static void tdbPCachePinPage(SPCache *pCache, SPage *pPage) {
  if (pPage->pLruNext != NULL) {
    ASSERT(tdbGetPageRef(pPage) == 0);

    pPage->pLruPrev->pLruNext = pPage->pLruNext;
    pPage->pLruNext->pLruPrev = pPage->pLruPrev;
    pPage->pLruNext = NULL;

    pCache->nRecyclable--;

    // printf("pin page %d pgno %d pPage %p\n", pPage->id, TDB_PAGE_PGNO(pPage), pPage);
    tdbDebug("pcache/pin page %p/%d/%d", pPage, TDB_PAGE_PGNO(pPage), pPage->id);
  }
}

static void tdbPCacheUnpinPage(SPCache *pCache, SPage *pPage) {
  i32 nRef;

  ASSERT(pPage->isLocal);
  ASSERT(!pPage->isDirty);
  ASSERT(tdbGetPageRef(pPage) == 0);

  ASSERT(pPage->pLruNext == NULL);

  pPage->pLruPrev = &(pCache->lru);
  pPage->pLruNext = pCache->lru.pLruNext;
  pCache->lru.pLruNext->pLruPrev = pPage;
  pCache->lru.pLruNext = pPage;

  pCache->nRecyclable++;

  // printf("unpin page %d pgno %d pPage %p\n", pPage->id, TDB_PAGE_PGNO(pPage), pPage);
  tdbDebug("pcache/unpin page %p/%d/%d", pPage, TDB_PAGE_PGNO(pPage), pPage->id);
}

static void tdbPCacheRemovePageFromHash(SPCache *pCache, SPage *pPage) {
  uint32_t h = tdbPCachePageHash(&(pPage->pgid)) % pCache->nHash;

  SPage **ppPage = &(pCache->pgHash[h]);
  for (; (*ppPage) && *ppPage != pPage; ppPage = &((*ppPage)->pHashNext))
    ;

  if (*ppPage) {
    *ppPage = pPage->pHashNext;
    pCache->nPage--;
    // printf("rmv page %d to hash, pgno %d, pPage %p\n", pPage->id, TDB_PAGE_PGNO(pPage), pPage);
  }

  tdbDebug("pcache/remove page %p/%d/%d from hash %" PRIu32, pPage, TDB_PAGE_PGNO(pPage), pPage->id, h);
}

static void tdbPCacheAddPageToHash(SPCache *pCache, SPage *pPage) {
  uint32_t h = tdbPCachePageHash(&(pPage->pgid)) % pCache->nHash;

  pPage->pHashNext = pCache->pgHash[h];
  pCache->pgHash[h] = pPage;

  pCache->nPage++;

  // printf("add page %d to hash, pgno %d, pPage %p\n", pPage->id, TDB_PAGE_PGNO(pPage), pPage);
  tdbDebug("pcache/add page %p/%d/%d to hash %" PRIu32, pPage, TDB_PAGE_PGNO(pPage), pPage->id, h);
}

static int tdbPCacheOpenImpl(SPCache *pCache) {
  SPage *pPage;
  u8    *pPtr;
  int    tsize;
  int    ret;

  tdbPCacheInitLock(pCache);

  // Open the free list
  pCache->nFree = 0;
  pCache->pFree = NULL;
  for (int i = 0; i < pCache->nPages; i++) {
    if (tdbPageCreate(pCache->szPage, &pPage, tdbDefaultMalloc, NULL) < 0) {
      // TODO: handle error
      return -1;
    }

    // pPage->pgid = 0;
    pPage->isAnchor = 0;
    pPage->isLocal = 1;
    pPage->nRef = 0;
    pPage->pHashNext = NULL;
    pPage->pLruNext = NULL;
    pPage->pLruPrev = NULL;
    pPage->pDirtyNext = NULL;

    // add page to free list
    pPage->pFreeNext = pCache->pFree;
    pCache->pFree = pPage;
    pCache->nFree++;

    // add to local list
    pPage->id = i;
    pCache->aPage[i] = pPage;
  }

  // Open the hash table
  pCache->nPage = 0;
  pCache->nHash = pCache->nPages < 8 ? 8 : pCache->nPages;
  pCache->pgHash = (SPage **)tdbOsCalloc(pCache->nHash, sizeof(SPage *));
  if (pCache->pgHash == NULL) {
    // TODO
    return -1;
  }

  // Open LRU list
  pCache->nRecyclable = 0;
  pCache->lru.isAnchor = 1;
  pCache->lru.pLruNext = &(pCache->lru);
  pCache->lru.pLruPrev = &(pCache->lru);

  return 0;
}

static int tdbPCacheCloseImpl(SPCache *pCache) {
  for (i32 iPage = 0; iPage < pCache->nPages; iPage++) {
    if (pCache->aPage[iPage]) {
      tdbPageDestroy(pCache->aPage[iPage], tdbDefaultFree, NULL);
      pCache->aPage[iPage] = NULL;
    }
  }

  tdbOsFree(pCache->pgHash);
  tdbPCacheDestroyLock(pCache);
  return 0;
}
