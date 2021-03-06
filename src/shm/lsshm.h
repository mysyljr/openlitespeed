/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2015  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#ifndef LSSHM_H
#define LSSHM_H

#include <lsdef.h>
#include <shm/lsshmlock.h>
#include <shm/lsshmtypes.h>
#include <util/hashstringmap.h>

#include <assert.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @file
 * LsShm - LiteSpeed Shared Memory
 */

#define SHM_DBG(format, ...) \
    LOG4CXX_NS::Logger::getRootLogger()->debug(format, ##__VA_ARGS__ )

#define SHM_NOTICE(format, ...) \
    LOG4CXX_NS::Logger::getRootLogger()->notice(format, ##__VA_ARGS__ )


#ifdef LSSHM_DEBUG_ENABLE
class debugBase;// These two should be the same size...
#endif
class LsShmPool;
class LsShmHash;
class LsShmWLruHash;
class LsShmXLruHash;

#define LSSHM_LRU_NONE      0
#define LSSHM_LRU_MODE1     1
#define LSSHM_LRU_MODE2     2
#define LSSHM_LRU_MODE3     3

typedef union
{
    struct
    {
        uint8_t     m_iMajor;
        uint8_t     m_iMinor;
        uint8_t     m_iRel;
        uint8_t     m_iType;
    } x;
    uint32_t    m_iVer;
} LsShmVersion;

//
// LiteSpeed Shared Memory Map
//
//  +------------------------------------------------
//  | HEADER
//  |    --> maxSize      --------------------------+
//  |    --> offset free  ----------------+         |
//  |    --> offset xdata ------+         |         |
//  |           size of xdata   |         |         |
//  |-------------------------- |         |         |
//  | xdata area          <-----+         |         |
//  |-------------------------            |         |
//  | Free area           <---------------+         |
//  |-------------------------  <-------------------+
//
// struct mutex_t;


//
//  SHM Reg
// NOTE: ALL UNIT 0 of each block is for book keeping purpose
//  Simple key/value pair
//  But this is expandable... Not a good idea to have more than 64 entries...
//  (1) Should be reference via regNumber
//  (2) Can be reference via name (but slow).
//  (3) should sizeof LsShmReg == LsShmRegBlkHdr
//
typedef struct
{
    uint16_t          x_iRegNum;   // max 64k registry
    uint16_t          x_iFlag;     // 0x1 - assigned
    LsShmOffset_t     x_iValue;    // typical offset
    uint8_t           x_aName[LSSHM_MAXNAMELEN];
} LsShmReg;

typedef struct
{
    uint16_t          x_iRegNum;
    uint16_t          x_iFlag;
    uint16_t          x_iCapacity; // max
    uint16_t          x_iSize;     // current size
    LsShmOffset_t     x_iNext;     // next block
    LsShmCnt_t        x_iStartNum; // start reg number
} LsShmRegBlkHdr;

typedef union
{
    LsShmRegBlkHdr    x_hdr;
    LsShmReg          x_reg;
} LsShmRegElem;

// x_iFlag
#define LSSHM_REGFLAG_CLR       0x0000
#define LSSHM_REGFLAG_SET       0x0001
#define LSSHM_REGFLAG_PID       0x0002

// NOTE: offset element[0] == header
#define LSSHM_REGPERBLK     (LSSHM_SHM_UNITSIZE/sizeof(LsShmRegElem))
typedef struct
{
    LsShmRegElem x_aEntries[LSSHM_REGPERBLK];
} LsShmRegBlk;

typedef struct
{
    LsShmXSize_t      m_iFileSize;          // file size (bytes)
    LsShmXSize_t      m_iUsedSize;          // file size allocated (bytes)
    LsShmXSize_t      m_iAllocated;         // total allocated (blocks)
    LsShmXSize_t      m_iReleased;          // total released (blocks)
    LsShmXSize_t      m_iFreeListCnt;       // entries on free list
} LsShmMapStat;
// NOTE: UsedSize - Allocated + Released = FreeList bytes

typedef struct
{
    uint32_t          x_iMagic;
    LsShmVersion      x_version;
    uint64_t          x_id;
    uint8_t           x_aName[LSSHM_MAXNAMELEN];
    LsShmOffset_t     x_iLockOffset[2];     // 0=Shm, 1=Reg
    LsShmOffset_t     x_iFreeOffset;
    LsShmOffset_t     x_iRegBlkOffset;      // 1st reg Offset
    LsShmOffset_t     x_iRegLastBlkOffset;  // last reg Offset
    LsShmCnt_t        x_iMaxRegNum;         // current max Reg number
    LsShmMapStat      x_stat;               // map statistics
} LsShmMap;

//
// Internal for free link
//
#define LSSHM_FREE_AMARKER 0x0123fedc
#define LSSHM_FREE_BMARKER 0xba984567
//
//  Two block to indicate a free block
//
typedef struct
{
    uint32_t         x_iAMarker;
    LsShmSize_t      x_iFreeSize;           // size of the freeblock
    LsShmOffset_t    x_iFreeNext;
    LsShmOffset_t    x_iFreePrev;
} LShmFreeTop;

typedef struct
{
    LsShmOffset_t    x_iFreeOffset;         // back to the begin
    uint32_t         x_iBMarker;
} LShmFreeBot;

/*
 *   data struct all the library routines to access the map
 */
class LsShm : public ls_shm_s
{
public:
    static LsShm *open(const char *mapName, LsShmXSize_t initSize,
                       const char *pBaseDir = NULL);
    void close()
    {
        if (downRef() == 0)
            delete this;
    }

    void deleteFile();


private:
    LsShm();
    ~LsShm();

public:
    static LsShmStatus_t setSysShmDir(const char *dirName);
    static const char *sysShmDir()
    {
        return s_pDirBase;
    }

    static const char *getDefaultShmDir();

    static LsShmStatus_t setErrMsg(LsShmStatus_t stat, const char *fmt, ...);
    static LsShmStatus_t getErrStat()
    {
        return s_errStat;
    }
    static int getErrNo()
    {
        return s_iErrNo;
    }
    static const char *getErrMsg()
    {
        return s_aErrMsg;
    }
    static void clrErrMsg()
    {
        s_errStat = LSSHM_OK;
        s_iErrNo = 0;
        s_aErrMsg[0] = '\0';
    }

    void setShmMaxSize(LsShmXSize_t size)
    {
        m_iMaxShmSize = size > LSSHM_MINSPACE ? size : LSSHM_MINSPACE;
    }
    LsShmXSize_t shmMaxSize() const
    {
        return m_iMaxShmSize;
    }

    LsShmXSize_t maxSize() const
    {
        return x_pShmMap->x_stat.m_iFileSize;
    }

    LsShmXSize_t oldMaxSize() const
    {
        return m_iMaxSizeO;
    };

    const char *fileName() const
    {
        return m_pFileName;
    }

    const char *name() const
    {
        return x_pShmMap ? (char *)x_pShmMap->x_aName : NULL;
    }

    const char *mapName() const
    {
        return m_pMapName;
    }

    ls_attr_inline LsShmMap *getShmMap() const
    {
        return x_pShmMap;
    }

    LsShmOffset_t getMapStatOffset() const
    {
        return (LsShmOffset_t)(long) & ((LsShmMap *)0)->x_stat;
    }

    LsShmOffset_t allocPage(LsShmSize_t pagesize, int &remapped);
    void releasePage(LsShmOffset_t offset, LsShmSize_t pagesize);

    LsShmPool *getGlobalPool();
    LsShmPool *getNamedPool(const char *pName);


    // dangerous... no check fast access
    void *offset2ptr(LsShmOffset_t offset) const
    {
        if (offset == 0)
            return NULL;
        assert(offset < m_iMaxSizeO);
        return (void *)(((uint8_t *)x_pShmMap) + offset);
    }  // map size

    LsShmOffset_t ptr2offset(const void *ptr) const
    {
        if (ptr == NULL)
            return 0;
        assert((ptr < (((uint8_t *)x_pShmMap) + x_pShmMap->x_stat.m_iFileSize))
               && (ptr > (const void *)x_pShmMap));
        return (LsShmOffset_t)((uint8_t *)ptr - (uint8_t *)x_pShmMap);
    }

    LsShmXSize_t avail() const
    {
        return x_pShmMap->x_stat.m_iFileSize - x_pShmMap->x_stat.m_iUsedSize;
    }

    LsShmStatus_t status() const
    {
        return m_status;
    }

    LsShmOffset_t xdataOffset() const
    {
        return (LsShmOffset_t)s_iShmHdrSize;
    };

    LsShmLock *lockPool()
    {
        return &m_locks;
    };

    lsi_shmlock_t *allocLock()
    {   return m_locks.allocLock();         }

    int freeLock(lsi_shmlock_t *pLock)
    {   return m_locks.freeLock(pLock);     }

    lsi_shmlock_t *offset2pLock(LsShmOffset_t offset) const
    {   return m_locks.offset2pLock(offset);    }

    LsShmOffset_t pLock2offset(lsi_shmlock_t *pLock)
    {   return m_locks.pLock2offset(pLock);     }


    ls_attr_inline int lockRemap(lsi_shmlock_t *pLock)
    {
        int ret = lsi_shmlock_lock(pLock);
        chkRemap();
        return ret;
    }

    ls_attr_inline LsShmStatus_t chkRemap()
    {
        return (x_pShmMap->x_stat.m_iFileSize == m_iMaxSizeO) ? LSSHM_OK : remap();
    }

    LsShmStatus_t remap();

    //
    //   registry
    //
    LsShmReg      *getReg(int regNum);  // user should check name and x_iFlag
    int            clrReg(int regNum);
    LsShmReg      *findReg(const char *name);
    LsShmReg      *addReg(const char *name);
    void           expandReg();         // expand the current reg

    int            recoverOrphanShm();

    int ref() const
    {
        return m_iRef;
    }

    HashStringMap < lsi_shmobject_t *> &getObjBase()
    {
        return m_objBase;
    }

#ifdef LSSHM_DEBUG_ENABLE
    // for debug purpose
    friend class debugBase;
#endif

private:
    LsShm(const LsShm &other);
    LsShm &operator=(const LsShm &other);
    bool operator==(const LsShm &other);

    LsShmRegBlk    *allocRegBlk(LsShmRegBlkHdr *pFrom);
    LsShmRegBlk    *findRegBlk(LsShmRegBlkHdr *pFrom, int num);
    void            chkRegBlk(LsShmRegBlkHdr *pFrom, int *pCnt);

    LsShmStatus_t   expand(LsShmXSize_t incrSize);

    ls_attr_inline int lock()
    {
        return lockRemap(m_pShmLock);
    }

    int unlock()
    {
        return lsi_shmlock_unlock(m_pShmLock);
    }

    int lockReg()
    {
        return lsi_shmlock_lock(m_pRegLock);
    }

    int unlockReg()
    {
        return lsi_shmlock_unlock(m_pRegLock);
    }

    LsShmStatus_t setupLocks()
    {
        return (lsi_shmlock_setup(m_pShmLock) || lsi_shmlock_setup(m_pRegLock)) ?
               LSSHM_ERROR : LSSHM_OK;
    }

    // only use by physical mapping
    LsShmXSize_t roundToPageSize(LsShmXSize_t size) const
    {
        return ((size + s_iPageSize - 1) / s_iPageSize) * s_iPageSize;
    }

    LsShmSize_t roundUnitSize(LsShmSize_t pagesize) const
    {
        return ((pagesize + (LSSHM_SHM_UNITSIZE - 1))
                / LSSHM_SHM_UNITSIZE) * LSSHM_SHM_UNITSIZE;
    }

    void incrCheck(LsShmXSize_t *ptr, LsShmSize_t size)
    {
        LsShmXSize_t prev = *ptr;
        *ptr += size;
        if (*ptr < prev)    // cnt wrapped
            *ptr = (LsShmSize_t) - 1;
        return;
    }

    void used(LsShmSize_t size)
    {
        x_pShmMap->x_stat.m_iUsedSize += size;
    }

    bool isFreeBlockAbove(LsShmOffset_t offset, LsShmSize_t size,
                          int joinFlag);
    bool isFreeBlockBelow(LsShmOffset_t offset, LsShmSize_t size,
                          int joinFlag);
    void joinFreeList(LsShmOffset_t offset, LsShmSize_t size);

    LsShmOffset_t getFromFreeList(LsShmSize_t pagesize);
    void reduceFreeFromBot(LShmFreeTop *ap,
                           LsShmOffset_t offset, LsShmSize_t newsize);
    void disconnectFromFree(LShmFreeTop *ap, LShmFreeBot *bp);
    void markTopUsed(LShmFreeTop *ap)
    {
        ap->x_iAMarker = 0;
    }

    void            cleanup();
    LsShmStatus_t   checkMagic(LsShmMap *mp, const char *mName) const;
    LsShmStatus_t   initShm(const char *mapName, LsShmXSize_t initialSize,
                            const char *pBaseDir = NULL);
    LsShmStatus_t   openLockShmFile();

    LsShmStatus_t   map(LsShmXSize_t size);
    LsShmStatus_t   expandFile(LsShmOffset_t from, LsShmXSize_t incrSize);
    void            unmap();

#ifdef DELAY_UNMAP
    void            unmapLater();
    void            unmapHelper(LsShmMap *, LsShmSize_t);
    void            unmapCheck();

    typedef struct
    {
        LsShmMap    *p;
        LsShmSize_t  size;
        timeval      tv;
    } unmapStat_t;
#define MAX_UNMAP 0x10
    unmapStat_t     m_uArray[MAX_UNMAP];
    unmapStat_t    *m_uCur;
    unmapStat_t    *m_uEnd;
#endif

    int upRef()
    {
        return ++m_iRef;
    }
    int downRef()
    {
        return --m_iRef;
    }


    // various objects within the SHM
    HashStringMap < lsi_shmobject_t *> m_objBase;

    LsShmLock               m_locks;
    uint32_t                m_iMagic;
    static LsShmVersion     s_version;
    static LsShmSize_t      s_iPageSize;
    static LsShmSize_t      s_iShmHdrSize;
    static const char      *s_pDirBase;
    static LsShmStatus_t    s_errStat;
    static int              s_iErrNo;
    static char             s_aErrMsg[];
    LsShmXSize_t            m_iMaxShmSize;

    LsShmStatus_t           m_status;
    char                   *m_pFileName;    // dir + mapName + ext
    char                   *m_pMapName;
    int                     m_iFd;
    lsi_shmlock_t          *m_pShmLock;
    lsi_shmlock_t          *m_pRegLock;

    LsShmMap               *x_pShmMap;
    LsShmMap               *m_pShmMapO;
    LsShmXSize_t            m_iMaxSizeO;

    LsShmPool              *m_pGPool;
    int                     m_iRef;
};

#endif
