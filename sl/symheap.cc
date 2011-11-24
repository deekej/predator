/*
 * Copyright (C) 2009-2011 Kamil Dudka <kdudka@redhat.com>
 * Copyright (C) 2010 Petr Peringer, FIT
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "symheap.hh"

#include <cl/cl_msg.hh>
#include <cl/clutil.hh>
#include <cl/storage.hh>

#include "intarena.hh"
#include "symabstract.hh"
#include "syments.hh"
#include "sympred.hh"
#include "symseg.hh"
#include "symutil.hh"
#include "symtrace.hh"
#include "util.hh"
#include "worklist.hh"

#ifndef NDEBUG
    // just for debugging purposes
#   include "symcmp.hh"
#endif

#include <algorithm>
#include <map>
#include <set>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>

template <class TCont> typename TCont::value_type::second_type&
assignInvalidIfNotFound(
        TCont                                           &cont,
        const typename TCont::value_type::first_type    &item)
{
    if (!hasKey(cont, item))
        // -1 means "invalid", e.g. VAL_INVALID in case [T = map<???, TValId>]
        cont[item] = static_cast<typename TCont::value_type::second_type>(-1);

    return cont[item];
}

// /////////////////////////////////////////////////////////////////////////////
// Neq predicates store
class NeqDb: public SymPairSet<TValId, /* IREFLEXIVE */ true> {
    public:
        RefCounter refCnt;

    public:
        template <class TDst>
        void gatherRelatedValues(TDst &dst, TValId val) const {
            // FIXME: suboptimal due to performance
            BOOST_FOREACH(const TItem &item, cont_) {
                if (item.first == val)
                    dst.push_back(item.second);
                else if (item.second == val)
                    dst.push_back(item.first);
            }
        }

        friend void SymHeapCore::copyRelevantPreds(
                SymHeapCore             &dst,
                const TValMap           &vMap)
            const;

        friend bool SymHeapCore::matchPreds(
                const SymHeapCore       &src,
                const TValMap           &vMap)
            const;
};

// /////////////////////////////////////////////////////////////////////////////
// CVar lookup container
class CVarMap {
    public:
        RefCounter refCnt;

    private:
        typedef std::map<CVar, TValId>              TCont;
        TCont                                       cont_;

    public:
        void insert(CVar cVar, TValId val) {
            // check for mapping redefinition
            CL_BREAK_IF(hasKey(cont_, cVar));

            // define mapping
            cont_[cVar] = val;
        }

        void remove(CVar cVar) {
            if (1 != cont_.erase(cVar))
                CL_BREAK_IF("offset detected in CVarMap::remove()");
        }

        TValId find(const CVar &cVar) {
            // regular lookup
            TCont::iterator iter = cont_.find(cVar);
            const bool found = (cont_.end() != iter);
            if (!cVar.inst) {
                // gl variable explicitly requested
                return (found)
                    ? iter->second
                    : VAL_INVALID;
            }

            // automatic fallback to gl variable
            CVar gl = cVar;
            gl.inst = /* global variable */ 0;
            TCont::iterator iterGl = cont_.find(gl);
            const bool foundGl = (cont_.end() != iterGl);

            if (!found && !foundGl)
                // not found anywhere
                return VAL_INVALID;

            // check for clash on uid among lc/gl variable
            CL_BREAK_IF(found && foundGl);

            if (found)
                return iter->second;
            else /* if (foundGl) */
                return iterGl->second;
        }
};


// /////////////////////////////////////////////////////////////////////////////
// implementation of CustomValue
bool operator==(const CustomValue &a, const CustomValue &b) {
    const ECustomValue code = a.code;
    if (b.code != code)
        return false;

    switch (code) {
        case CV_INVALID:
            return true;

        case CV_FNC:
            return (a.data.uid == b.data.uid);

        case CV_INT:
            return (a.data.num == b.data.num);

        case CV_REAL:
            return (a.data.fpn == b.data.fpn);

        case CV_STRING:
            return STREQ(a.data.str, b.data.str);

        case CV_INT_RANGE:
            return (a.data.rng == b.data.rng);
    }

    CL_BREAK_IF("CustomValue::operator==() got something special");
    return false;
}


// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeapCore
typedef std::set<TObjId>                                TObjIdSet;
typedef std::map<TOffset, TValId>                       TOffMap;
typedef IntervalArena<TOffset, TObjId>                  TArena;
typedef TArena::key_type                                TMemChunk;
typedef TArena::value_type                              TMemItem;

inline TMemItem createArenaItem(
        const TOffset               off,
        const unsigned              size,
        const TObjId                obj)
{
    const TMemChunk chunk(off, off + size);
    return TMemItem(chunk, obj);
}

inline bool arenaLookup(
        TObjIdSet                   *dst,
        const TArena                &arena,
        const TMemChunk             &chunk,
        const TObjId                obj)
{
    arena.intersects(*dst, chunk);

    if (OBJ_INVALID != obj)
        // remove the reference object itself
        dst->erase(obj);

    // finally check if there was anything else
    return !dst->empty();
}

inline void arenaLookForExactMatch(
        TObjIdSet                   *dst,
        const TArena                &arena,
        const TMemChunk             &chunk)
{
    arena.exactMatch(*dst, chunk);
}

// create a right-open interval
inline TMemChunk createChunk(const TOffset off, const TObjType clt) {
    CL_BREAK_IF(!clt || clt->code == CL_TYPE_VOID);
    return TMemChunk(off, off + clt->size);
}

enum EBlockKind {
    BK_INVALID,
    BK_DATA_PTR,
    BK_DATA_OBJ,
    BK_COMPOSITE,
    BK_UNIFORM
};

typedef std::map<TObjId, EBlockKind>                    TLiveObjs;

inline EBlockKind bkFromClt(const TObjType clt) {
    if (isComposite(clt, /* includingArray */ false))
        return BK_COMPOSITE;

    return (isDataPtr(clt))
        ? BK_DATA_PTR
        : BK_DATA_OBJ;
}

class AbstractHeapEntity {
    public:
        virtual AbstractHeapEntity* clone() const = 0;

    protected:
        virtual ~AbstractHeapEntity() { }
        friend class EntStore<AbstractHeapEntity>;
        friend class RefCntLibBase;
        friend class RefCntLib<RCO_VIRTUAL>;

    private:
        RefCounter refCnt;

        // intentionally not implemented
        AbstractHeapEntity& operator=(const AbstractHeapEntity &);
};

struct BlockEntity: public AbstractHeapEntity {
    EBlockKind                  code;
    TValId                      root;
    TOffset                     off;
    TOffset                     size;
    TValId                      value;

    BlockEntity(
            const EBlockKind        code_,
            const TValId            root_,
            const TOffset           off_,
            const TOffset           size_,
            const TValId            value_):
        code(code_),
        root(root_),
        off(off_),
        size(size_),
        value(value_)
    {
    }

    virtual BlockEntity* clone() const {
        return new BlockEntity(*this);
    }
};

struct HeapObject: public BlockEntity {
    TObjType                    clt;
    int                         extRefCnt;

    HeapObject(TValId root_, TOffset off_, TObjType clt_):
        BlockEntity(bkFromClt(clt_), root_, off_, clt_->size, VAL_INVALID),
        clt(clt_),
        extRefCnt(0)
    {
    }

    virtual HeapObject* clone() const {
        return new HeapObject(*this);
    }
};

struct BaseValue: public AbstractHeapEntity {
    EValueTarget                    code;
    EValueOrigin                    origin;
    TValId                          valRoot;
    TOffset                         offRoot;
    TObjIdSet                       usedBy;
    TValId                          anchor;

    BaseValue(EValueTarget code_, EValueOrigin origin_):
        code(code_),
        origin(origin_),
        offRoot(0)
    {
    }

    virtual BaseValue* clone() const {
        return new BaseValue(*this);
    }
};

/// maintains a list of dependent values
struct ReferableValue: public BaseValue {
    TValList                        dependentValues;

    ReferableValue(EValueTarget code_, EValueOrigin origin_):
        BaseValue(code_, origin_)
    {
    }

    virtual ReferableValue* clone() const {
        return new ReferableValue(*this);
    }
};

struct AnchorValue: public ReferableValue {
    TOffMap                         offMap;

    AnchorValue(EValueTarget code_, EValueOrigin origin_):
        ReferableValue(code_, origin_)
    {
    }

    virtual AnchorValue* clone() const {
        return new AnchorValue(*this);
    }
};

struct RangeValue: public AnchorValue {
    IR::Range                       range;

    RangeValue(const IR::Range &range_):
        AnchorValue(VT_RANGE, VO_ASSIGNED),
        range(range_)
    {
    }

    virtual RangeValue* clone() const {
        return new RangeValue(*this);
    }
};

struct CompValue: public BaseValue {
    TObjId                          compObj;

    CompValue(EValueTarget code_, EValueOrigin origin_):
        BaseValue(code_, origin_)
    {
    }

    virtual BaseValue* clone() const {
        return new CompValue(*this);
    }
};

struct InternalCustomValue: public ReferableValue {
    CustomValue                     customData;

    InternalCustomValue(EValueTarget code_, EValueOrigin origin_):
        ReferableValue(code_, origin_)
    {
    }

    virtual InternalCustomValue* clone() const {
        return new InternalCustomValue(*this);
    }
};

struct RootValue: public AnchorValue {
    CVar                            cVar;
    TOffset                         size;
    TLiveObjs                       liveObjs;
    TObjIdSet                       usedByGl;
    TArena                          arena;
    TObjType                        lastKnownClt;
    bool                            isProto;

    RootValue(EValueTarget code_, EValueOrigin origin_):
        AnchorValue(code_, origin_),
        size(0),
        lastKnownClt(0),
        isProto(false)
    {
    }

    virtual RootValue* clone() const {
        return new RootValue(*this);
    }
};

class CustomValueMapper {
    private:
        typedef std::map<int, TValId>                           TCustomByInt;
        typedef std::map<IR::TInt, TValId>                      TCustomByLong;
        typedef std::map<double, TValId>                        TCustomByReal;
        typedef std::map<std::string, TValId>                   TCustomByString;

        TCustomByInt        fncMap;
        TCustomByLong       numMap;
        TCustomByReal       fpnMap;
        TCustomByString     strMap;
        TValId              inval_;

    public:
        RefCounter          refCnt;

    public:
        TValId& lookup(const CustomValue &item) {
            const ECustomValue code = item.code;
            switch (code) {
                case CV_INVALID:
                default:
                    CL_BREAK_IF("invalid call of CustomValueMapper::lookup()");
                    return inval_ = VAL_INVALID;

                case CV_FNC:
                    return assignInvalidIfNotFound(fncMap, item.data.uid);

                case CV_INT:
                    return assignInvalidIfNotFound(numMap, item.data.num);

                case CV_REAL:
                    return assignInvalidIfNotFound(fpnMap, item.data.fpn);

                case CV_STRING:
                    return assignInvalidIfNotFound(strMap, item.data.str);
            }
        }
};

// FIXME: std::set is not a good candidate for base class
struct TValSetWrapper: public TValSet {
    RefCounter refCnt;
};

struct CoincidenceDb {
    RefCounter                      refCnt;
    SymPairMap<TValId, bool>        db;
};

struct SymHeapCore::Private {
    Private(Trace::Node *);
    Private(const Private &);
    ~Private();

    Trace::NodeHandle               traceHandle;
    EntStore<AbstractHeapEntity>    ents;
    TValSetWrapper                 *liveRoots;
    CVarMap                        *cVarMap;
    CustomValueMapper              *cValueMap;
    CoincidenceDb                  *coinDb;
    NeqDb                          *neqDb;

    inline TObjId assignId(BlockEntity *);
    inline TValId assignId(BaseValue *);

    TValId valCreate(EValueTarget code, EValueOrigin origin);
    TValId valDup(TValId);
    bool valsEqual(TValId, TValId);

    TObjId objCreate(TValId root, TOffset off, TObjType clt);
    TValId objInit(TObjId obj);
    void objDestroy(TObjId, bool removeVal, bool detach);

    TObjId copySingleLiveBlock(
            const TValId            rootDst,
            RootValue              *rootDataDst,
            const TObjId            objSrc,
            const EBlockKind        code,
            const TOffset           shift = 0);

    TValId dupRoot(TValId root);
    void destroyRoot(TValId obj);

    bool /* wasPtr */ releaseValueOf(TObjId obj, TValId val);
    void registerValueOf(TObjId obj, TValId val);
    void splitBlockByObject(TObjId block, TObjId obj);
    void reinterpretObjData(TObjId old, TObjId obj, TValSet *killedPtrs = 0);
    void setValueOf(TObjId of, TValId val, TValSet *killedPtrs = 0);

    // runs only in debug build
    bool chkArenaConsistency(const RootValue *);

    void shiftBlockAt(
            const TValId            dstRoot,
            const TOffset           off,
            const TOffset           size,
            const TValSet          *killedPtrs);

    void transferBlock(
            const TValId            dstRoot,
            const TValId            srcRoot, 
            const TOffset           dstOff,
            const TOffset           srcOff,
            const TOffset           size);

    TObjId writeUniformBlock(
            const TValId            addr,
            const TValId            tplValue,
            const unsigned          size,
            TValSet                 *killedPtrs);

    void bindValues(TValId v1, TValId v2, bool neg);

    TValId shiftCustomValue(TValId val, TOffset shift);

    void replaceRngByInt(const InternalCustomValue *valData);

    void trimCustomValue(TValId val, const IR::Range &win);

    private:
        // intentionally not implemented
        Private& operator=(const Private &);
};

inline TValId SymHeapCore::Private::assignId(BaseValue *valData) {
    const TValId val = this->ents.assignId<TValId>(valData);
    valData->valRoot = val;
    valData->anchor  = val;
    return val;
}

inline TObjId SymHeapCore::Private::assignId(BlockEntity *hbData) {
    return this->ents.assignId<TObjId>(hbData);
}

bool /* wasPtr */ SymHeapCore::Private::releaseValueOf(TObjId obj, TValId val) {
    if (val <= 0)
        // we do not track uses of special values
        return /* wasPtr */ false;

    BaseValue *valData;
    this->ents.getEntRW(&valData, val);
    TObjIdSet &usedBy = valData->usedBy;
    if (1 != usedBy.erase(obj))
        CL_BREAK_IF("SymHeapCore::Private::releaseValueOf(): offset detected");

    if (usedBy.empty()) {
        // kill all related Neq predicates
        TValList neqs;
        this->neqDb->gatherRelatedValues(neqs, val);
        BOOST_FOREACH(const TValId valNeq, neqs) {
            CL_DEBUG("releaseValueOf() kills an orphan Neq predicate");
            this->neqDb->del(valNeq, val);
        }
    }

    const EValueTarget code = valData->code;
    if (!isAnyDataArea(code))
        return /* wasPtr */ false;

    // jump to root
    const TValId root = valData->valRoot;
    this->ents.getEntRW(&valData, root);

    RootValue *rootData = DCAST<RootValue *>(valData);
    if (1 != rootData->usedByGl.erase(obj))
        CL_BREAK_IF("SymHeapCore::Private::releaseValueOf(): offset detected");

    return /* wasPtr */ true;
}

void SymHeapCore::Private::registerValueOf(TObjId obj, TValId val) {
    if (val <= 0)
        return;

    // update usedBy
    BaseValue *valData;
    this->ents.getEntRW(&valData, val);
    valData->usedBy.insert(obj);

    const EValueTarget code = valData->code;
    if (!isAnyDataArea(code))
        return;

    // update usedByGl
    const TValId root = valData->valRoot;
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);
    rootData->usedByGl.insert(obj);
}

// runs only in debug build
bool SymHeapCore::Private::chkArenaConsistency(const RootValue *rootData) {
    TLiveObjs all(rootData->liveObjs);
    if (isGone(rootData->code)) {
        CL_BREAK_IF(rootData->size);
        CL_BREAK_IF(!rootData->liveObjs.empty());

        // we can check nothing for VT_DELETED/VT_LOST, we do not know the size
        return true;
    }

    const TArena &arena = rootData->arena;
    const TMemChunk chunk(0, rootData->size);

    TObjIdSet overlaps;
    if (arenaLookup(&overlaps, arena, chunk, OBJ_INVALID)) {
        BOOST_FOREACH(const TObjId obj, overlaps)
            all.erase(obj);
    }

    if (all.empty())
        return true;

    CL_WARN("live object not mapped in arena: #" << all.begin()->first);
    return false;
}

void SymHeapCore::Private::splitBlockByObject(
        TObjId                      block,
        TObjId                      obj)
{
    BlockEntity *blData;
    this->ents.getEntRW(&blData, block);

    const BlockEntity *hbData;
    this->ents.getEntRO(&hbData, obj);

    const EBlockKind code = hbData->code;
    switch (code) {
        case BK_DATA_PTR:
        case BK_DATA_OBJ:
            if (this->valsEqual(blData->value, hbData->value))
                // preserve non-conflicting uniform blocks
                return;

        default:
            break;
    }

    // dig root
    const TValId root = blData->root;
    CL_BREAK_IF(root != hbData->root);

    // check up to now arena consistency
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);
    CL_BREAK_IF(!this->chkArenaConsistency(rootData));

    // dig offsets and sizes
    const TOffset blOff = blData->off;
    const TOffset objOff = hbData->off;
    const unsigned blSize = blData->size;
    const unsigned objSize = hbData->size;

    // check overlapping
    const TOffset blBegToObjBeg = objOff - blOff;
    const TOffset objEndToBlEnd = blSize - objSize - blBegToObjBeg;

    if (blBegToObjBeg <= 0 && objEndToBlEnd <= 0) {
        // block completely overlapped by the object, throw it away
        if (!rootData->liveObjs.erase(block))
            CL_BREAK_IF("attempt to kill an already dead uniform block");

        rootData->arena -= createArenaItem(blOff, blSize, block);
        this->ents.releaseEnt(block);
        return;
    }

    if (0 < blBegToObjBeg && 0 < objEndToBlEnd) {
        // the object is strictly in the middle of the block (needs split)
        BlockEntity *blDataOther = blData->clone();
        const TObjId blOther = this->assignId(blDataOther);

        // update metadata
        blData->size = blBegToObjBeg;
        blDataOther->size = objEndToBlEnd;
        blDataOther->off = objOff + objSize;

        // unmap part of the original block
        rootData->arena -= createArenaItem(
                blOff + blBegToObjBeg,
                objSize + objEndToBlEnd,
                block);

        // map the new block
        rootData->arena += createArenaItem(
                objOff + objSize,
                objEndToBlEnd,
                blOther);

        rootData->liveObjs[blOther] = BK_UNIFORM;
        return;
    }

    // check direction
    const TOffset diff = blOff - objOff;
    const bool shiftBeg = (0 <= diff);
    const TOffset beg = (shiftBeg)
        ? /* shift begin of the block */ blOff
        : /* trim end of the block    */ objOff;

    // compute size of the overlapping region
    const TOffset trim = (shiftBeg)
        ? (objSize - diff)
        : (blSize + /* negative */ diff);

    // throw away the overlapping part of the block
    blData->size -= trim;
    if (shiftBeg)
        blData->off += trim;

    // unmap the overlapping part
    CL_BREAK_IF(trim <= 0 || !blData->size);
    rootData->arena -= createArenaItem(beg, trim, block);
}

bool isCoveredByBlock(
        const HeapObject           *objData,
        const BlockEntity          *blData)
{
    const TOffset beg1 = objData->off;
    const TOffset beg2 = blData->off;
    if (beg1 < beg2)
        // the object starts above the block
        return false;

    const TOffset end1 = beg1 + objData->clt->size;
    const TOffset end2 = beg2 + blData->size;
    return (end1 <= end2);
}

void SymHeapCore::Private::reinterpretObjData(
        TObjId                      old,
        TObjId                      obj,
        TValSet                    *killedPtrs)
{
    BlockEntity *blData;
    this->ents.getEntRW(&blData, old);

    EBlockKind code = blData->code;
    switch (code) {
        case BK_DATA_PTR:
        case BK_DATA_OBJ:
            break;

        case BK_COMPOSITE:
            // do not invalidate those place-holding values of composite objects
            return;

        case BK_UNIFORM:
            this->splitBlockByObject(/* block */ old, obj);
            return;

        case BK_INVALID:
        default:
            CL_BREAK_IF("invalid call of reinterpretObjData()");
            return;
    }

    CL_DEBUG("reinterpretObjData() is taking place...");
    HeapObject *oldData = DCAST<HeapObject *>(blData);
    const TValId valOld = oldData->value;
    if (/* wasPtr */ this->releaseValueOf(old, valOld) && killedPtrs)
        killedPtrs->insert(valOld);

    // dig root
    const TValId root = oldData->root;
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);
    CL_BREAK_IF(!this->chkArenaConsistency(rootData));

    // mark the object as dead
    if (rootData->liveObjs.erase(old))
        CL_DEBUG("reinterpretObjData() kills a live object");

    if (!oldData->extRefCnt) {
        CL_DEBUG("reinterpretObjData() destroys a dead object");
        this->objDestroy(old, /* removeVal */ false, /* detach */ true);
        return;
    }

    CL_DEBUG("an object being reinterpreted is still referenced from outside");
    this->ents.getEntRW(&blData, obj);
    code = blData->code;

    TValId val;

    switch (code) {
        case BK_UNIFORM:
            if (isCoveredByBlock(oldData, blData)) {
                // object fully covered by the overlapping uniform block
                val = this->valDup(blData->value);
                break;
            }
            // fall through!

        case BK_DATA_PTR:
        case BK_DATA_OBJ:
            // TODO: hook various reinterpretation drivers here
            val = this->valCreate(VT_UNKNOWN, VO_REINTERPRET);
            break;

        case BK_COMPOSITE:
        case BK_INVALID:
        default:
            CL_BREAK_IF("invalid call of reinterpretObjData()");
            return;
    }

    // assign the value to the _old_ object
    oldData->value = val;
    this->registerValueOf(old, val);
}

void SymHeapCore::Private::setValueOf(
        TObjId                      obj,
        TValId                      val,
        TValSet                    *killedPtrs)
{
    // release old value
    HeapObject *objData;
    this->ents.getEntRW(&objData, obj);

    const TValId valOld = objData->value;
    if (/* wasPtr */ this->releaseValueOf(obj, valOld) && killedPtrs)
        killedPtrs->insert(valOld);

    // store new value
    objData->value = val;
    this->registerValueOf(obj, val);

    // resolve root
    const TValId root = objData->root;
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);

    // (re)insert self into the arena if not there
    TArena &arena = rootData->arena;
    const TOffset off = objData->off;
    const TObjType clt = objData->clt;
    arena += createArenaItem(off, clt->size, obj);

    // invalidate contents of the objects we are overwriting
    TObjIdSet overlaps;
    if (arenaLookup(&overlaps, arena, createChunk(off, clt), obj)) {
        BOOST_FOREACH(const TObjId old, overlaps)
            this->reinterpretObjData(old, obj, killedPtrs);
    }

    CL_BREAK_IF(!this->chkArenaConsistency(rootData));
}

TObjId SymHeapCore::Private::objCreate(
        TValId                      root,
        TOffset                     off,
        TObjType                    clt)
{
    // acquire object ID
    HeapObject *objData = new HeapObject(root, off, clt);
    const TObjId obj = this->assignId(objData);

    // register the object by the owning root value
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);

    // map the region occupied by the object
    rootData->arena += createArenaItem(off, clt->size, obj);
    CL_BREAK_IF(!this->chkArenaConsistency(rootData));
    return obj;
}

void SymHeapCore::Private::objDestroy(TObjId obj, bool removeVal, bool detach) {
    BlockEntity *blData;
    this->ents.getEntRW(&blData, obj);

    const EBlockKind code = blData->code;
    if (removeVal && BK_UNIFORM != code) {
        // release value of the object
        TValId &val = blData->value;
        this->releaseValueOf(obj, val);
        val = VAL_INVALID;
    }

    if (detach) {
        // properly remove the object from grid and arena
        RootValue *rootData;
        this->ents.getEntRW(&rootData, blData->root);
        CL_BREAK_IF(!this->chkArenaConsistency(rootData));

        // remove the object from arena unless we are destroying everything
        const TOffset off = blData->off;
        const TOffset size = blData->size;
        rootData->arena -= createArenaItem(off, size, obj);

        CL_BREAK_IF(hasKey(rootData->liveObjs, obj));
        CL_BREAK_IF(!this->chkArenaConsistency(rootData));
    }

    if (BK_UNIFORM != code && 0 < DCAST<HeapObject *>(blData)->extRefCnt)
        // preserve an externally referenced object
        return;

    // release the corresponding HeapObject instance
    this->ents.releaseEnt(obj);
}

TValId SymHeapCore::Private::valCreate(
        EValueTarget                code,
        EValueOrigin                origin)
{
    TValId val = VAL_INVALID;

    switch (code) {
        case VT_INVALID:
        case VT_UNKNOWN:
            val = this->assignId(new BaseValue(code, origin));
            break;

        case VT_COMPOSITE:
            val = this->assignId(new CompValue(code, origin));
            break;

        case VT_CUSTOM:
            val = this->assignId(new InternalCustomValue(code, origin));
            break;

        case VT_RANGE:
        case VT_ABSTRACT:
            CL_BREAK_IF("invalid call of SymHeapCore::Private::valCreate()");
            // fall through!

        case VT_ON_HEAP:
        case VT_ON_STACK:
        case VT_STATIC:
        case VT_DELETED:
        case VT_LOST:
            val = this->assignId(new RootValue(code, origin));
            break;
    }

    return val;
}

TValId SymHeapCore::Private::valDup(TValId val) {
    if (val <= 0)
        // do not clone special values
        return val;

    // deep copy the value
    const BaseValue *tpl;
    this->ents.getEntRO(&tpl, val);
    BaseValue *dupData = /* FIXME: subtle */ tpl->clone();

    const TValId dup = this->assignId(dupData);

    // wipe BaseValue::usedBy
    dupData->usedBy.clear();

    return dup;
}

// FIXME: copy/pasted in symutil.hh
bool SymHeapCore::Private::valsEqual(TValId v1, TValId v2) {
    if (v1 == v2)
        // matches trivially
        return true;

    if (v1 <= 0 || v2 <= 0)
        // special values have to match
        return false;

    const BaseValue *valData1;
    const BaseValue *valData2;

    this->ents.getEntRO(&valData1, v1);
    this->ents.getEntRO(&valData2, v2);

    const EValueTarget code1 = valData1->code;
    const EValueTarget code2 = valData2->code;

    if (VT_UNKNOWN != code1 || VT_UNKNOWN != code2)
        // for now, we handle only unknown values here
        return false;

    CL_BREAK_IF(valData1->offRoot || valData2->offRoot);

    // just compare kinds of unknown values
    return (valData1->origin == valData2->origin);
}

void SymHeapCore::Private::shiftBlockAt(
        const TValId                dstRoot,
        const TOffset               off,
        const TOffset               size,
        const TValSet              *killedPtrs)
{
    CL_BREAK_IF("please implement");
    (void) dstRoot;
    (void) off;
    (void) size;
    (void) killedPtrs;
}

void SymHeapCore::Private::transferBlock(
        const TValId                dstRoot,
        const TValId                srcRoot, 
        const TOffset               dstOff,
        const TOffset               winBeg,
        const TOffset               winSize)
{
    const RootValue *rootDataSrc;
    this->ents.getEntRO(&rootDataSrc, srcRoot);
    const TArena &arena = rootDataSrc->arena;
    const TOffset winEnd = winBeg + winSize;
    const TMemChunk chunk (winBeg, winEnd);

    TObjIdSet overlaps;
    if (!arenaLookup(&overlaps, arena, chunk, OBJ_INVALID))
        // no data to copy in here
        return;

    RootValue *rootDataDst;
    this->ents.getEntRW(&rootDataDst, dstRoot);
    const TOffset shift = dstOff - winBeg;

    // go through overlaps and copy the live ones
    BOOST_FOREACH(const TObjId objSrc, overlaps) {
        const BlockEntity *hbDataSrc;
        this->ents.getEntRO(&hbDataSrc, objSrc);

        const TOffset beg = hbDataSrc->off;
        if (beg < winBeg)
            // the object starts above the window, do not copy this one
            continue;

        const TOffset end = beg + hbDataSrc->size;
        if (winEnd < end)
            // the object ends beyond the window, do not copy this one
            continue;

        const TLiveObjs &liveSrc = rootDataSrc->liveObjs;
        const TLiveObjs::const_iterator it = liveSrc.find(objSrc);
        if (liveSrc.end() == it)
            // dead object anyway
            continue;

        // copy a single live block
        const EBlockKind code = it->second;
        this->copySingleLiveBlock(dstRoot, rootDataDst, objSrc, code, shift);
    }
}


SymHeapCore::Private::Private(Trace::Node *trace):
    traceHandle (trace),
    liveRoots   (new TValSetWrapper),
    cVarMap     (new CVarMap),
    cValueMap   (new CustomValueMapper),
    coinDb      (new CoincidenceDb),
    neqDb       (new NeqDb)
{
    // allocate a root-value for VAL_NULL
    this->assignId(new RootValue(VT_INVALID, VO_INVALID));
}

SymHeapCore::Private::Private(const SymHeapCore::Private &ref):
    traceHandle (new Trace::CloneNode(ref.traceHandle.node())),
    ents        (ref.ents),
    liveRoots   (ref.liveRoots),
    cVarMap     (ref.cVarMap),
    cValueMap   (ref.cValueMap),
    coinDb      (ref.coinDb),
    neqDb       (ref.neqDb)
{
    RefCntLib<RCO_NON_VIRT>::enter(this->liveRoots);
    RefCntLib<RCO_NON_VIRT>::enter(this->cVarMap);
    RefCntLib<RCO_NON_VIRT>::enter(this->cValueMap);
    RefCntLib<RCO_NON_VIRT>::enter(this->coinDb);
    RefCntLib<RCO_NON_VIRT>::enter(this->neqDb);
}

SymHeapCore::Private::~Private() {
    RefCntLib<RCO_NON_VIRT>::leave(this->liveRoots);
    RefCntLib<RCO_NON_VIRT>::leave(this->cVarMap);
    RefCntLib<RCO_NON_VIRT>::leave(this->cValueMap);
    RefCntLib<RCO_NON_VIRT>::leave(this->coinDb);
    RefCntLib<RCO_NON_VIRT>::leave(this->neqDb);
}

TValId SymHeapCore::Private::objInit(TObjId obj) {
    HeapObject *objData;
    this->ents.getEntRW(&objData, obj);
    CL_BREAK_IF(!objData->extRefCnt);

    // resolve root
    const TValId root = objData->root;
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);
    CL_BREAK_IF(!this->chkArenaConsistency(rootData));

    const TArena &arena = rootData->arena;
    const TOffset off = objData->off;
    const TObjType clt = objData->clt;

    // first check for data reinterpretation
    TObjIdSet overlaps;
    if (arenaLookup(&overlaps, arena, createChunk(off, clt), obj)) {
        BOOST_FOREACH(const TObjId other, overlaps) {
            const BlockEntity *blockData;
            this->ents.getEntRO(&blockData, other);

            const EBlockKind code = blockData->code;
            if (BK_UNIFORM != code && !hasKey(rootData->liveObjs, other))
                continue;

            // reinterpret _self_ by another live object or uniform block
            this->reinterpretObjData(/* old */ obj, other);
            CL_BREAK_IF(!this->chkArenaConsistency(rootData));
            return objData->value;
        }
    }

    // assign a fresh unknown value
    const TValId val = this->valCreate(VT_UNKNOWN, VO_UNKNOWN);
    objData->value = val;

    // mark the object as live
    if (isDataPtr(clt))
        rootData->liveObjs[obj] = BK_DATA_PTR;
#if SE_TRACK_NON_POINTER_VALUES
    else
        rootData->liveObjs[obj] = BK_DATA_OBJ;
#endif

    CL_BREAK_IF(!this->chkArenaConsistency(rootData));

    // store backward reference
    BaseValue *valData;
    this->ents.getEntRW(&valData, val);
    valData->usedBy.insert(obj);
    return val;
}

TValId SymHeapCore::valueOf(TObjId obj) {
    // handle special cases first
    switch (obj) {
        case OBJ_UNKNOWN:
            // not implemented
        case OBJ_INVALID:
            return VAL_INVALID;

        case OBJ_DEREF_FAILED:
            return d->valCreate(VT_UNKNOWN, VO_DEREF_FAILED);

        default:
            break;
    }

    const HeapObject *objData;
    d->ents.getEntRO(&objData, obj);

    TValId val = objData->value;
    if (VAL_INVALID != val)
        // the object has a value
        return val;

    const TObjType clt = objData->clt;
    if (isComposite(clt)) {
        // deleayed creation of a composite value
        val = d->valCreate(VT_COMPOSITE, VO_INVALID);
        CompValue *compData;
        d->ents.getEntRW(&compData, val);
        compData->compObj = obj;

        // store the value
        HeapObject *objDataRW;
        d->ents.getEntRW(&objDataRW, obj);
        objDataRW->value = val;

        // store backward reference
        compData->usedBy.insert(obj);
        return val;
    }

    // delayed object initialization
    return d->objInit(obj);
}

void SymHeapCore::usedBy(ObjList &dst, TValId val, bool liveOnly) const {
    if (VAL_NULL == val)
        // we do not track uses of special values
        return;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    const TObjIdSet &usedBy = valData->usedBy;
    if (!liveOnly) {
        // dump everything
        BOOST_FOREACH(const TObjId obj, usedBy)
            dst.push_back(ObjHandle(*const_cast<SymHeapCore *>(this), obj));

        return;
    }

    BOOST_FOREACH(const TObjId obj, usedBy) {
        // get object data
        const HeapObject *objData;
        d->ents.getEntRO(&objData, obj);

        // get root data
        const TValId root = objData->root;
        const RootValue *rootData;
        d->ents.getEntRO(&rootData, root);

        // check if the object is alive
        if (hasKey(rootData->liveObjs, obj))
            dst.push_back(ObjHandle(*const_cast<SymHeapCore *>(this), obj));
    }
}

unsigned SymHeapCore::usedByCount(TValId val) const {
    if (VAL_NULL == val)
        return 0;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    return valData->usedBy.size();
}

void SymHeapCore::pointedBy(ObjList &dst, TValId root) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    CL_BREAK_IF(rootData->offRoot);
    CL_BREAK_IF(!isPossibleToDeref(rootData->code));

    const TObjIdSet &usedBy = rootData->usedByGl;
    BOOST_FOREACH(const TObjId obj, usedBy)
        dst.push_back(ObjHandle(*const_cast<SymHeapCore *>(this), obj));
}

unsigned SymHeapCore::pointedByCount(TValId root) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    return rootData->usedByGl.size();
}

unsigned SymHeapCore::lastId() const {
    return d->ents.lastId<unsigned>();
}

TValId SymHeapCore::valClone(TValId val) {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);

    const EValueTarget code = valData->code;
    if (VT_CUSTOM == code) {
        CL_BREAK_IF("custom values are not supposed to be cloned");
        return val;
    }

    if (isProgramVar(code)) {
        CL_BREAK_IF("program variables are not supposed to be cloned");
        return val;
    }

    const TValId root = valData->valRoot;
    if (VAL_NULL == root) {
        CL_BREAK_IF("VAL_NULL is not supposed to be cloned");
        return val;
    }

    if (VT_RANGE == code) {
        CL_DEBUG("support for VT_RANGE in valClone() is experimental");
        const IR::Range range = this->valOffsetRange(val);
        return this->valByRange(valData->valRoot, range);
    }

    if (!isPossibleToDeref(code))
        // duplicate an unknown value
        return d->valDup(val);

    // duplicate a root object
    const TValId dupAt = d->dupRoot(root);

    // take the offset into consideration
    return this->valByOffset(dupAt, valData->offRoot);
}

TObjId SymHeapCore::Private::copySingleLiveBlock(
        const TValId                rootDst,
        RootValue                  *rootDataDst,
        const TObjId                objSrc,
        const EBlockKind            code,
        const TOffset               shift)
{
    TObjId dst;

    if (BK_UNIFORM == code) {
        // duplicate a uniform block
        const BlockEntity *blSrc;
        this->ents.getEntRO(&blSrc, objSrc);
        BlockEntity *blDst = blSrc->clone();
        dst = this->assignId(blDst);
        blDst->root = rootDst;

        // shift the block if asked to do so
        blDst->off += shift;

        // map the cloned block
        rootDataDst->arena += createArenaItem(blDst->off, blDst->size, dst);
    }
    else {
        // duplicate a regular object
        CL_BREAK_IF(BK_DATA_PTR != code && BK_DATA_OBJ != code);

        const HeapObject *objDataSrc;
        this->ents.getEntRO(&objDataSrc, objSrc);

        const TOffset off = objDataSrc->off + shift;
        const TObjType clt = objDataSrc->clt;
        dst = this->objCreate(rootDst, off, clt);
        this->setValueOf(dst, objDataSrc->value);
    }

    // prevserve live object code
    rootDataDst->liveObjs[dst] = code;
    return dst;
}

TValId SymHeapCore::Private::dupRoot(TValId rootAt) {
    CL_DEBUG("SymHeapCore::Private::dupRoot() is taking place...");
    const RootValue *rootDataSrc;
    this->ents.getEntRO(&rootDataSrc, rootAt);
    CL_BREAK_IF(!this->chkArenaConsistency(rootDataSrc));

    // assign an address to the clone
    const EValueTarget code = rootDataSrc->code;
    const TValId imageAt = this->valCreate(code, VO_ASSIGNED);
    RootValue *rootDataDst;
    this->ents.getEntRW(&rootDataDst, imageAt);

    // duplicate root metadata
    rootDataDst->cVar               = rootDataSrc->cVar;
    rootDataDst->size               = rootDataSrc->size;
    rootDataDst->lastKnownClt       = rootDataSrc->lastKnownClt;
    rootDataDst->isProto            = rootDataSrc->isProto;

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(this->liveRoots);
    this->liveRoots->insert(imageAt);

    BOOST_FOREACH(TLiveObjs::const_reference item, rootDataSrc->liveObjs)
        this->copySingleLiveBlock(imageAt, rootDataDst,
                /* src  */ item.first,
                /* code */ item.second);

    CL_BREAK_IF(!this->chkArenaConsistency(rootDataDst));
    return imageAt;
}

void SymHeapCore::gatherLivePointers(ObjList &dst, TValId root) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);

    BOOST_FOREACH(TLiveObjs::const_reference item, rootData->liveObjs) {
        const EBlockKind code = item.second;
        if (BK_DATA_PTR != code)
            continue;

        const TObjId obj = item.first;
        dst.push_back(ObjHandle(*const_cast<SymHeapCore *>(this), obj));
    }
}

void SymHeapCore::gatherUniformBlocks(TUniBlockMap &dst, TValId root) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    BOOST_FOREACH(TLiveObjs::const_reference item, rootData->liveObjs) {
        const EBlockKind code = item.second;
        if (BK_UNIFORM != code)
            continue;

        const BlockEntity *blData;
        d->ents.getEntRO(&blData, /* obj */ item.first);
        const TOffset off = blData->off;
        CL_BREAK_IF(hasKey(dst, off));
        UniformBlock &block = dst[off];

        // export uniform block
        block.off       = off;
        block.size      = blData->size;
        block.tplValue  = blData->value;
    }
}

void SymHeapCore::gatherLiveObjects(ObjList &dst, TValId root) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);

    BOOST_FOREACH(TLiveObjs::const_reference item, rootData->liveObjs) {
        const EBlockKind code = item.second;

        switch (code) {
            case BK_UNIFORM:
                continue;

            case BK_DATA_PTR:
            case BK_DATA_OBJ:
                break;

            case BK_INVALID:
            default:
                CL_BREAK_IF("gatherLiveObjects sees something special");
        }

        const TObjId obj = item.first;
        dst.push_back(ObjHandle(*const_cast<SymHeapCore *>(this), obj));
    }
}

bool SymHeapCore::findCoveringUniBlock(
        UniformBlock                *pDst,
        const TValId                root,
        const TOffset               beg,
        unsigned                    size)
    const
{
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    CL_BREAK_IF(!d->chkArenaConsistency(rootData));

    const TArena &arena = rootData->arena;
    const TOffset end = beg + size;
    const TMemChunk chunk(beg, end);

    TObjIdSet overlaps;
    if (!arenaLookup(&overlaps, arena, chunk, OBJ_INVALID))
        // not found
        return false;

    BOOST_FOREACH(const TObjId id, overlaps) {
        const BlockEntity *data;
        d->ents.getEntRO(&data, id);

        const EBlockKind code = data->code;
        if (BK_UNIFORM != code)
            continue;

        const BlockEntity *blData = DCAST<const BlockEntity *>(data);

        const TOffset blBeg = blData->off;
        if (beg < blBeg)
            // the template starts above this block
            continue;

        const TOffset size = blData->size;
        const TOffset blEnd = blBeg + size;
        if (blEnd < end)
            // the template ends beyond this block
            continue;

        // covering uniform block matched!
        pDst->off       = blBeg;
        pDst->size      = size;
        pDst->tplValue  = blData->value;
        return true;
    }

    // not found
    return false;
}

SymHeapCore::SymHeapCore(TStorRef stor, Trace::Node *trace):
    stor_(stor),
    d(new Private(trace))
{
    CL_BREAK_IF(!&stor_);

    // initialize VAL_ADDR_OF_RET
    const TValId addrRet = d->valCreate(VT_ON_STACK, VO_ASSIGNED);
    CL_BREAK_IF(VAL_ADDR_OF_RET != addrRet);
    (void) addrRet;
}

SymHeapCore::SymHeapCore(const SymHeapCore &ref):
    stor_(ref.stor_),
    d(new Private(*ref.d))
{
    CL_BREAK_IF(!&stor_);
}

SymHeapCore::~SymHeapCore() {
    delete d;
}

SymHeapCore& SymHeapCore::operator=(const SymHeapCore &ref) {
    CL_BREAK_IF(&stor_ != &ref.stor_);

    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeapCore::swap(SymHeapCore &ref) {
    CL_BREAK_IF(&stor_ != &ref.stor_);
    swapValues(this->d, ref.d);
}

Trace::Node* SymHeapCore::traceNode() const {
    return d->traceHandle.node();
}

void SymHeapCore::traceUpdate(Trace::Node *node) {
    d->traceHandle.reset(node);
}

void SymHeapCore::objSetValue(TObjId obj, TValId val, TValSet *killedPtrs) {
    // we allow to set values of atomic types only
    const HeapObject *objData;
    d->ents.getEntRO(&objData, obj);

    const TObjType clt = objData->clt;
    CL_BREAK_IF(isComposite(clt, /* includingArray */ false));
    CL_BREAK_IF(isComposite(clt) && objData->off);

    // check whether the root entity that owns this object ID is still valid
    CL_BREAK_IF(!isPossibleToDeref(this->valTarget(objData->root)));

    // mark the destination object as live
    const TValId root = objData->root;
    RootValue *rootData;
    d->ents.getEntRW(&rootData, root);
    rootData->liveObjs[obj] = bkFromClt(clt);

    // now set the value
    d->setValueOf(obj, val, killedPtrs);
}

TObjId SymHeapCore::Private::writeUniformBlock(
        const TValId                addr,
        const TValId                tplVal,
        const unsigned              size,
        TValSet                     *killedPtrs)
{
    const BaseValue *valData;
    this->ents.getEntRO(&valData, addr);

    const TValId root = valData->valRoot;
    const TOffset beg = valData->offRoot;
    const TOffset end = beg + size;

    // acquire object ID
    BlockEntity *blData = new BlockEntity(BK_UNIFORM, root, beg, size, tplVal);
    const TObjId obj = this->assignId(blData);

    // check up to now arena consistency
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);
    CL_BREAK_IF(!this->chkArenaConsistency(rootData));

    // mark the block as live
    rootData->liveObjs[obj] = BK_UNIFORM;

    TArena &arena = rootData->arena;
    arena += createArenaItem(beg, size, obj);
    const TMemChunk chunk(beg, end);

    // invalidate contents of the objects we are overwriting
    TObjIdSet overlaps;
    if (arenaLookup(&overlaps, arena, chunk, obj)) {
        BOOST_FOREACH(const TObjId old, overlaps)
            this->reinterpretObjData(old, obj, killedPtrs);
    }

    CL_BREAK_IF(!this->chkArenaConsistency(rootData));
    return obj;
}

/// just a trivial wrapper to hide the return value
void SymHeapCore::writeUniformBlock(
        const TValId                addr,
        const TValId                tplValue,
        const unsigned              size,
        TValSet                     *killedPtrs)
{
    CL_BREAK_IF(this->valSizeOfTarget(addr) < static_cast<int>(size));
    d->writeUniformBlock(addr, tplValue, size, killedPtrs);
}

void SymHeapCore::copyBlockOfRawMemory(
        const TValId                dst,
        const TValId                src,
        const unsigned              size,
        TValSet                     *killedPtrs)
{
    // this should have been checked by the caller
    CL_BREAK_IF(this->valSizeOfTarget(dst) < static_cast<int>(size));
    CL_BREAK_IF(this->valSizeOfTarget(src) < static_cast<int>(size));

    const BaseValue *dstData;
    const BaseValue *srcData;

    d->ents.getEntRO(&dstData, dst);
    d->ents.getEntRO(&srcData, src);

    CL_BREAK_IF(!isPossibleToDeref(dstData->code));
    CL_BREAK_IF(!isPossibleToDeref(srcData->code));
    CL_BREAK_IF(!size);

    const TOffset dstOff = dstData->offRoot;
    const TOffset srcOff = srcData->offRoot;
    const TValId dstRoot = dstData->valRoot;
    const TValId srcRoot = srcData->valRoot;

    if (dstRoot == srcRoot) {
        // movement within a single root entity
        const TOffset diff = dstOff - srcOff;
        d->shiftBlockAt(dstRoot, diff, size, killedPtrs);
        return;
    }

    // nuke the content we are going to overwrite
    const TObjId blKiller = d->writeUniformBlock(dst, /* misleading */ VAL_NULL,
                                                 size, killedPtrs);

    // remove the dummy block we used just to trigger the data reinterpretation
    RootValue *rootDataDst;
    d->ents.getEntRW(&rootDataDst, dstRoot);
    rootDataDst->liveObjs.erase(blKiller);
    rootDataDst->arena -= createArenaItem(dstOff, size, blKiller);
    d->ents.releaseEnt(blKiller);

    // now we need to transfer data between two distinct root entities
    d->transferBlock(dstRoot, srcRoot, dstOff, srcOff, size);
}

TObjType SymHeapCore::objType(TObjId obj) const {
    if (obj < 0)
        return 0;

    const HeapObject *objData;
    d->ents.getEntRO(&objData, obj);
    return objData->clt;
}

// FIXME: this feature needs to be better documented
TValId SymHeapCore::Private::shiftCustomValue(TValId ref, TOffset shift) {
    const InternalCustomValue *customDataRef;
    this->ents.getEntRO(&customDataRef, ref);

    CL_BREAK_IF(CV_INT_RANGE != customDataRef->customData.code);
    const IR::Range &rngRef = customDataRef->customData.data.rng;

    // prepare a custom value template and compute the shifted range
    CustomValue cv(CV_INT_RANGE);
    cv.data.rng = rngRef + IR::rngFromNum(shift);

    // create a new CV_INT_RANGE custom value (do not recycle existing)
    const TValId val = this->valCreate(VT_CUSTOM, VO_ASSIGNED);
    InternalCustomValue *customData;
    this->ents.getEntRW(&customData, val);
    customData->anchor      = customDataRef->anchor;
    customData->customData  = cv;

    // register this value as a dependent value by the anchor
    ReferableValue *refData;
    this->ents.getEntRW(&refData, customData->anchor);
    refData->dependentValues.push_back(val);

    return val;
}

void SymHeapCore::Private::replaceRngByInt(const InternalCustomValue *valData) {
    CL_DEBUG("replaceRngByInt() is taking place...");

    // we already expect CV_INT at this point
    const CustomValue &cvRng = valData->customData;
    CL_BREAK_IF(CV_INT != cvRng.code);

    TValId replaceBy;
    if (0L == cvRng.data.num)
        replaceBy = VAL_NULL;
    else if (1L == cvRng.data.num)
        replaceBy = VAL_TRUE;
    else {
        // CV_INT values are supposed to be reused if they exist already
        RefCntLib<RCO_NON_VIRT>::requireExclusivity(this->cValueMap);
        TValId &valInt = this->cValueMap->lookup(cvRng);
        if (VAL_INVALID == valInt) {
            // CV_INT_RANGE not found, wrap it as a new heap value
            valInt = this->valCreate(VT_CUSTOM, VO_ASSIGNED);
            InternalCustomValue *intData;
            this->ents.getEntRW(&intData, valInt);
            intData->customData = cvRng;
        }
        replaceBy = valInt;
    }

    // we intentionally do not use a reference here (tight loop otherwise)
    TObjIdSet usedBy = valData->usedBy;
    BOOST_FOREACH(const TObjId obj, usedBy)
        this->setValueOf(obj, replaceBy);
}

void SymHeapCore::Private::trimCustomValue(TValId val, const IR::Range &win) {
    const InternalCustomValue *customData;
    this->ents.getEntRO(&customData, val);

    // extract the custom value and check we get CV_INT_RANGE
    const CustomValue &cv = customData->customData;
    if (CV_INT_RANGE != cv.code) {
        CL_BREAK_IF("only CV_INT_RANGE custom values can be restricted");
        return;
    }

    // extract the original integral ragne
    const IR::Range &refRange = cv.data.rng;
    CL_BREAK_IF(isSingular(refRange));

    // compute the difference between the original and desired ranges
    const IR::TInt loShift = win.lo - refRange.lo;
    const IR::TInt hiShift = refRange.hi - win.hi;
    if (0 < loShift && hiShift < 0) {
        CL_BREAK_IF("attempt to use trimCustomValue() to enlarge the interval");
        return;
    }

    // jump to anchor
    const TValId anchor = customData->anchor;
    const ReferableValue *refData;
    this->ents.getEntRO(&refData, anchor);

    // go through all dependent values including the anchor itself
    TValList deps = refData->dependentValues;
    deps.push_back(anchor);
    BOOST_FOREACH(const TValId depVal, deps) {
        // FIXME: are custom values the only allowed dependent values here?
        InternalCustomValue *depData;
        this->ents.getEntRW(&depData, depVal);

        CustomValue &cvDep = depData->customData;
        CL_BREAK_IF(CV_INT_RANGE != cvDep.code);

        // shift the bounds accordingly
        IR::Range &rngDep = cvDep.data.rng;
        rngDep.lo -= loShift;
        rngDep.hi -= hiShift;

        if (isSingular(rngDep)) {
            // CV_INT_RANGE reduced to CV_INT
            cvDep.code = CV_INT;
            cvDep.data.num = rngDep.lo;
            this->replaceRngByInt(depData);
        }
    }
}

TValId SymHeapCore::valByOffset(TValId at, TOffset off) {
    if (!off || at < 0)
        return at;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, at);
    const TValId valRoot = valData->valRoot;
    const EValueTarget code = valData->code;

    TValId anchor = valRoot;
    if (VT_RANGE == code)
        anchor = valData->anchor;

    // subtract the root
    off += valData->offRoot;
    if (!off)
        return anchor;

    if (VT_UNKNOWN == code)
        // do not track off-value for invalid targets
        return d->valDup(at);

    if (VT_CUSTOM == code) {
        // FIXME: this feature needs to be better documented
        return d->shiftCustomValue(at, off);
    }

    // off-value lookup
    const AnchorValue *anchorData;
    d->ents.getEntRO(&anchorData, anchor);
    const TOffMap &offMap = anchorData->offMap;
    TOffMap::const_iterator it = offMap.find(off);
    if (offMap.end() != it)
        return it->second;

    // create a new off-value
    BaseValue *offVal = new BaseValue(code, valData->origin);
    const TValId val = d->assignId(offVal);

    // offVal->valRoot needs to be set after the call of Private::assignId()
    offVal->valRoot = valRoot;
    offVal->anchor  = anchor;
    offVal->offRoot = off;

    // store the mapping for next wheel
    AnchorValue *anchorDataRW;
    d->ents.getEntRW(&anchorDataRW, anchor);
    anchorDataRW->offMap[off] = val;
    return val;
}

TValId SymHeapCore::valByRange(TValId at, IR::Range range) {
    if (isSingular(range)) {
        CL_DEBUG("valByRange() got a singular range, passing to valByOffset()");
        return this->valByOffset(at, range.lo);
    }

    const BaseValue *valData;
    d->ents.getEntRO(&valData, at);
    if (VAL_NULL == at || isGone(valData->code))
        return d->valCreate(VT_UNKNOWN, VO_UNKNOWN);

    CL_BREAK_IF(!isPossibleToDeref(valData->code));

    // subtract the root offset
    const TValId valRoot = valData->valRoot;
    const TOffset offset = valData->offRoot;
    range += IR::rngFromNum(offset);

    // create a new range value
    RangeValue *rangeData = new RangeValue(range);
    const TValId val = d->assignId(rangeData);

    // offVal->valRoot needs to be set after the call of Private::assignId()
    rangeData->valRoot  = valRoot;
    rangeData->anchor   = val;

    // register the VT_RANGE value by the owning root entity
    RootValue *rootData;
    d->ents.getEntRW(&rootData, valRoot);
    rootData->dependentValues.push_back(val);

    return val;
}

void SymHeapCore::valRestrictRange(TValId val, IR::Range win) {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);

    const EValueTarget code = valData->code;
    switch (code) {
        case VT_RANGE:
            break;

        case VT_CUSTOM:
            d->trimCustomValue(val, win);
            return;

        case VT_UNKNOWN:
            if (!isSingular(win)) {
                CustomValue cv(CV_INT_RANGE);
                cv.data.rng = win;
                this->valReplace(val, this->valWrapCustom(cv));
                return;
            }
            // fall through!

        default:
            CL_BREAK_IF("invalid call of valRestrictRange()");
            return;
    }

    const TValId anchor = valData->anchor;
    const TOffset shift = valData->offRoot;
    CL_BREAK_IF((!!shift) == (anchor == val));

    RangeValue *rangeData;
    d->ents.getEntRW(&rangeData, anchor);
    IR::Range &range = rangeData->range;

    // translate the given window to our root coords
    win -= IR::rngFromNum(shift);

    // first check that the caller uses the SymHeapCore API correctly
    CL_BREAK_IF(win == range);
    CL_BREAK_IF(win.lo < range.lo);
    CL_BREAK_IF(range.hi < win.hi);

    // restrict the offset range now!
    range = win;
    if (!isSingular(range))
        return;

    // the range has been restricted to a single off-value, trow it away!
    CL_DEBUG("valRestrictRange() throws away a singular offset range...");
    const TValId valRoot = rangeData->valRoot;
    const TOffset offRoot = range.lo;
    const TValId valSubst = this->valByOffset(valRoot, offRoot);
    this->valReplace(anchor, valSubst);

    BOOST_FOREACH(TOffMap::const_reference item, rangeData->offMap) {
        const TOffset offRel = item.first;
        const TValId valOld = item.second;

        const TOffset offTotal = offRoot + offRel;
        const TValId valNew = this->valByOffset(valRoot, offTotal);
        this->valReplace(valOld, valNew);
    }
}

void SymHeapCore::Private::bindValues(TValId v1, TValId v2, bool neg) {
    const BaseValue *valData1, *valData2;
    this->ents.getEntRO(&valData1, v1);
    this->ents.getEntRO(&valData2, v2);

    const TValId anchor1 = valData1->anchor;
    const TValId anchor2 = valData2->anchor;

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(this->coinDb);
    this->coinDb->db.add(anchor1, anchor2, neg);
}

bool SymHeapCore::areBound(bool *pNeg, TValId v1, TValId v2) {
    const BaseValue *valData1, *valData2;
    d->ents.getEntRO(&valData1, v1);
    d->ents.getEntRO(&valData2, v2);

    const TValId anchor1 = valData1->anchor;
    const TValId anchor2 = valData2->anchor;

    if (d->coinDb->db.chk(pNeg, anchor1, anchor2))
        return true;

    CL_DEBUG("SymHeapCore::areBound() returns false");
    return false;
}

TValId SymHeapCore::diffPointers(const TValId v1, const TValId v2) {
    const TValId root1 = this->valRoot(v1);
    const TValId root2 = this->valRoot(v2);
    if (root1 != root2)
        return d->valCreate(VT_UNKNOWN, VO_UNKNOWN);

    // get offset ranges for both pointers
    const IR::Range off1 = this->valOffsetRange(v1);
    const IR::Range off2 = this->valOffsetRange(v2);

    // prepare a custom value for the result
    CustomValue cv(CV_INT_RANGE);
    IR::Range &diff = cv.data.rng;

    // TODO: check for an already existing coincidence to improve the precision

    // compute the difference and wrap it as a heap value
    diff = off1 - off2;

    const TValId valDiff = this->valWrapCustom(cv);
    if (isSingular(diff))
        // good luck, the difference is a scalar
        return valDiff;

    if (isSingular(off2))
        d->bindValues(valDiff, v1, /* neg */ false);

    if (isSingular(off1))
        d->bindValues(valDiff, v2, /* neg */ true);

    return valDiff;
}

EValueOrigin SymHeapCore::valOrigin(TValId val) const {
    switch (val) {
        case VAL_INVALID:
            return VO_INVALID;

        case VAL_NULL /* = VAL_FALSE */:
        case VAL_TRUE:
            return VO_ASSIGNED;

        default:
            break;
    }

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    return valData->origin;
}

EValueTarget SymHeapCore::valTarget(TValId val) const {
    if (val <= 0)
        return VT_INVALID;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);

    const EValueTarget code = valData->code;
    if (VT_RANGE == code)
        // VT_RANGE takes precedence over VT_ABSTRACT
        return VT_RANGE;

    if (this->hasAbstractTarget(val))
        // the overridden implementation claims the target is abstract
        return VT_ABSTRACT;

    // just return the native code we track in BaseValue
    return code;
}

bool isUninitialized(EValueOrigin code) {
    switch (code) {
        case VO_HEAP:
        case VO_STACK:
            return true;

        default:
            return false;
    }
}

bool isAbstract(EValueTarget code) {
    return (VT_ABSTRACT == code);
}

bool isKnownObject(EValueTarget code) {
    switch (code) {
        case VT_STATIC:
        case VT_ON_HEAP:
        case VT_ON_STACK:
            return true;

        default:
            return false;
    }
}

bool isGone(EValueTarget code) {
    switch (code) {
        case VT_DELETED:
        case VT_LOST:
            return true;

        default:
            return false;
    }
}

bool isOnHeap(EValueTarget code) {
    switch (code) {
        case VT_ON_HEAP:
        case VT_ABSTRACT:
            return true;

        default:
            return false;
    }
}

bool isProgramVar(EValueTarget code) {
    switch (code) {
        case VT_STATIC:
        case VT_ON_STACK:
            return true;

        default:
            return false;
    }
}

bool isPossibleToDeref(EValueTarget code) {
    return isOnHeap(code)
        || isProgramVar(code);
}

bool isAnyDataArea(EValueTarget code) {
    return isPossibleToDeref(code)
        || (VT_RANGE == code);
}

TValId SymHeapCore::valRoot(TValId val) const {
    if (val <= 0)
        return val;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    return valData->valRoot;
}

TOffset SymHeapCore::valOffset(TValId val) const {
    if (val <= 0)
        return 0;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    if (VT_RANGE == valData->code) {
        CL_BREAK_IF("valOffset() called on VT_RANGE, which is not supported");
        return -1;
    }

    return valData->offRoot;
}

IR::Range SymHeapCore::valOffsetRange(TValId val) const {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);

    if (VT_RANGE != valData->code)
        // this is going to be a singular range
        return IR::rngFromNum(valData->offRoot);

    const TValId anchor = valData->anchor;
    if (anchor == val) {
        // we got the VT_RANGE anchor directly
        const RangeValue *rangeData = DCAST<const RangeValue *>(valData);
        return rangeData->range;
    }

    // we need to resolve an off-value to VT_RANGE anchor
    const RangeValue *rangeData;
    d->ents.getEntRO(&rangeData, anchor);

    // check the offset we need to shift the anchor by
    const TOffset off = valData->offRoot;
    CL_BREAK_IF(!off);

    // shift the range (if not already saturated) and return the result
    IR::Range range = rangeData->range;
    range += IR::rngFromNum(off);
    return range;
}

void SymHeapCore::valReplace(TValId val, TValId replaceBy) {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);

    // kill all related Neq predicates
    TValList neqs;
    d->neqDb->gatherRelatedValues(neqs, val);
    BOOST_FOREACH(const TValId valNeq, neqs) {
        CL_BREAK_IF(valNeq == replaceBy);
        SymHeapCore::neqOp(NEQ_DEL, valNeq, val);
    }

    // we intentionally do not use a reference here (tight loop otherwise)
    TObjIdSet usedBy = valData->usedBy;
    BOOST_FOREACH(const TObjId obj, usedBy) {
        // this used to happen with with test-0037 running in OOM mode [fixed]
        CL_BREAK_IF(isGone(this->valTarget(this->placedAt(obj))));

        this->objSetValue(obj, replaceBy);
    }
}

void SymHeapCore::neqOp(ENeqOp op, TValId v1, TValId v2) {
    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d->neqDb);

    switch (op) {
        case NEQ_NOP:
            CL_BREAK_IF("invalid call of SymHeapCore::neqOp()");
            return;

        case NEQ_ADD:
            d->neqDb->add(v1, v2);
            return;

        case NEQ_DEL:
            d->neqDb->del(v1, v2);
            return;
    }
}

void SymHeapCore::gatherRelatedValues(TValList &dst, TValId val) const {
    d->neqDb->gatherRelatedValues(dst, val);
}

void SymHeapCore::copyRelevantPreds(SymHeapCore &dst, const TValMap &valMap)
    const
{
    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb->cont_) {
        TValId valLt = item.first;
        TValId valGt = item.second;

        if (!translateValId(&valLt, dst, *this, valMap))
            // not relevant
            continue;

        if (!translateValId(&valGt, dst, *this, valMap))
            // not relevant
            continue;

        // create the image now!
        dst.neqOp(NEQ_ADD, valLt, valGt);
    }
}

bool SymHeapCore::matchPreds(const SymHeapCore &ref, const TValMap &valMap)
    const
{
    SymHeapCore &src = const_cast<SymHeapCore &>(*this);
    SymHeapCore &dst = const_cast<SymHeapCore &>(ref);

    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb->cont_) {
        TValId valLt = item.first;
        TValId valGt = item.second;

        if (!translateValId(&valLt, dst, src, valMap))
            // failed to translate value ID, better to give up
            return false;

        if (!translateValId(&valGt, dst, src, valMap))
            // failed to translate value ID, better to give up
            return false;

        if (!ref.d->neqDb->chk(valLt, valGt))
            // Neq predicate not matched
            return false;
    }

    return true;
}

TValId SymHeapCore::placedAt(TObjId obj) {
    if (obj < 0)
        return VAL_INVALID;

    // jump to root
    const HeapObject *objData;
    d->ents.getEntRO(&objData, obj);
    const TValId root = objData->root;

    // then subtract the offset
    return this->valByOffset(root, objData->off);
}

TObjId SymHeapCore::ptrAt(TValId at) {
    if (at <= 0)
        return OBJ_INVALID;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, at);

    const EValueTarget code = valData->code;
    CL_BREAK_IF(VT_RANGE == code);
    if (!isPossibleToDeref(code))
        return OBJ_INVALID;

    // jump to root
    const TValId valRoot = valData->valRoot;
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, valRoot);

    // generic pointer, (void *) if available
    const TObjType clt = stor_.types.genericDataPtr();
    CL_BREAK_IF(!clt || clt->code != CL_TYPE_PTR);
    const TOffset size = clt->size;
    CL_BREAK_IF(size <= 0);

    // arena lookup
    TObjIdSet candidates;
    const TArena &arena = rootData->arena;
    const TOffset off = valData->offRoot;
    const TMemChunk chunk(off, off + size);
    arenaLookForExactMatch(&candidates, arena, chunk);

    // seek a _data_ pointer in the given interval
    BOOST_FOREACH(const TObjId obj, candidates) {
        const BlockEntity *blData;
        d->ents.getEntRO(&blData, obj);
        const EBlockKind code = blData->code;
        if (BK_DATA_PTR != code && BK_DATA_OBJ != code)
            continue;

        const HeapObject *objData = DCAST<const HeapObject *>(blData);
        const TObjType clt = objData->clt;
        if (isDataPtr(clt))
            return obj;
    }

    // check whether we have enough space allocated for the pointer
    if (this->valSizeOfTarget(at) < clt->size) {
        CL_BREAK_IF("ptrAt() called out of bounds");
        return OBJ_UNKNOWN;
    }

    // resolve root
    const TValId root = valData->valRoot;

    // create the pointer
    return d->objCreate(root, off, clt);
}

// TODO: simplify the code
TObjId SymHeapCore::objAt(TValId at, TObjType clt) {
    if (at <= 0)
        return OBJ_INVALID;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, at);

    const EValueTarget code = valData->code;
    CL_BREAK_IF(VT_RANGE == code);
    if (!isPossibleToDeref(code))
        return OBJ_INVALID;

    CL_BREAK_IF(!clt || !clt->size);
    const TOffset size = clt->size;

    // jump to root
    const TValId valRoot = valData->valRoot;
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, valRoot);

    // arena lookup
    TObjIdSet candidates;
    const TArena &arena = rootData->arena;
    const TOffset off = valData->offRoot;
    const TMemChunk chunk(off, off + size);
    arenaLookForExactMatch(&candidates, arena, chunk);

    TObjId bestMatch = OBJ_INVALID;
    bool liveObjFound = false;
    bool cltExactMatch = false;
    bool cltClassMatch = false;

    // go through the objects in the given interval
    BOOST_FOREACH(const TObjId obj, candidates) {
        const BlockEntity *blData;
        d->ents.getEntRO(&blData, obj);
        const EBlockKind code = blData->code;
        switch (code) {
            case BK_DATA_PTR:
            case BK_DATA_OBJ:
            case BK_COMPOSITE:
                break;

            default:
                continue;
        }

        const bool isLive = hasKey(rootData->liveObjs, obj);
        if (liveObjFound && !isLive)
            continue;

        const HeapObject *objData = DCAST<const HeapObject *>(blData);
        const TObjType cltNow = objData->clt;
        if (cltNow == clt) {
            // exact match
            if (isLive)
                return obj;

            CL_BREAK_IF(cltExactMatch);
            cltExactMatch = true;
            goto update_best;
        }

        if (cltExactMatch)
            continue;

        if (*cltNow == *clt) {
            cltClassMatch = true;
            goto update_best;
        }

        if (cltClassMatch)
            continue;

        if (!isDataPtr(cltNow) || !isDataPtr(clt))
            continue;
        // at least both are _data_ pointers at this point, update best match

update_best:
        liveObjFound = isLive;
        bestMatch = obj;
    }

    if (OBJ_INVALID != bestMatch)
        return bestMatch;

    if (this->valSizeOfTarget(at) < clt->size)
        // out of bounds
        return OBJ_UNKNOWN;

    // create the object
    const TValId root = valData->valRoot;
    return d->objCreate(root, off, clt);
}

void SymHeapCore::objEnter(TObjId obj) {
    HeapObject *objData;
    d->ents.getEntRW(&objData, obj);
    CL_BREAK_IF(objData->extRefCnt < 0);
    ++(objData->extRefCnt);
}

void SymHeapCore::objLeave(TObjId obj) {
    HeapObject *objData;
    d->ents.getEntRW(&objData, obj);
    CL_BREAK_IF(objData->extRefCnt < 1);
    if (--(objData->extRefCnt))
        // still externally referenced
        return;

#if SH_DELAYED_OBJECTS_DESTRUCTION
    return;
#endif

    if (isComposite(objData->clt, /* includingArray */ false)
            && VAL_INVALID != objData->value)
    {
        CL_DEBUG("SymHeapCore::objLeave() preserves a composite object");
        return;
    }

    const TValId root = objData->root;
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    if (!hasKey(rootData->liveObjs, obj)) {
        CL_DEBUG("SymHeapCore::objLeave() destroys a dead object");
        d->objDestroy(obj, /* removeVal */ true, /* detach */ true);
    }

    // TODO: pack the representation if possible
}

CVar SymHeapCore::cVarByRoot(TValId valRoot) const {
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, valRoot);
    return rootData->cVar;
}

TValId SymHeapCore::addrOfVar(CVar cv, bool createIfNeeded) {
    TValId addr = d->cVarMap->find(cv);
    if (0 < addr)
        return addr;

    if (!createIfNeeded)
        // the variable does not exist and we are not asked to create the var
        return VAL_INVALID;

    // lazy creation of a program variable
    const CodeStorage::Var &var = stor_.vars[cv.uid];
    if (!isOnStack(var))
        cv.inst = /* gl var */ 0;

    TObjType clt = var.type;
    CL_BREAK_IF(!clt || clt->code == CL_TYPE_VOID);
#if DEBUG_SE_STACK_FRAME
    const struct cl_loc *loc = 0;
    std::string varString = varToString(stor_, cv.uid, &loc);
    CL_DEBUG_MSG(loc, "FFF SymHeapCore::addrOfVar() creates var " << varString);
#endif

    // assign an address
    const EValueTarget code = isOnStack(var) ? VT_ON_STACK : VT_STATIC;
    addr = d->valCreate(code, VO_ASSIGNED);

    RootValue *rootData;
    d->ents.getEntRW(&rootData, addr);
    rootData->cVar = cv;
    rootData->lastKnownClt = clt;

    // read size from the type-info
    const unsigned size = clt->size;
    rootData->size = size;

    // mark the root as live
    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d->liveRoots);
    d->liveRoots->insert(addr);

    // store the address for next wheel
    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d->cVarMap);
    d->cVarMap->insert(cv, addr);
    return addr;
}

static bool dummyFilter(EValueTarget) {
    return true;
}

void SymHeapCore::gatherRootObjects(TValList &dst, bool (*filter)(EValueTarget))
    const
{
    if (!filter)
        filter = dummyFilter;

    const TValSetWrapper &roots = *d->liveRoots;
    BOOST_FOREACH(const TValId at, roots)
        if (filter(this->valTarget(at)))
            dst.push_back(at);
}

TObjId SymHeapCore::valGetComposite(TValId val) const {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    CL_BREAK_IF(VT_COMPOSITE != valData->code);

    const CompValue *compData = DCAST<const CompValue *>(valData);
    return compData->compObj;
}

TValId SymHeapCore::heapAlloc(int cbSize) {
    CL_BREAK_IF(cbSize <= 0);

    // assign an address
    const TValId addr = d->valCreate(VT_ON_HEAP, VO_ASSIGNED);

    // mark the root as live
    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d->liveRoots);
    d->liveRoots->insert(addr);

    // initialize meta-data
    RootValue *rootData;
    d->ents.getEntRW(&rootData, addr);
    rootData->size = cbSize;

    return addr;
}

void SymHeapCore::valDestroyTarget(TValId val) {
    if (VAL_NULL == val) {
        CL_BREAK_IF("SymHeapCore::valDestroyTarget() got VAL_NULL");
        return;
    }

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    if (valData->offRoot || !isPossibleToDeref(valData->code)) {
        CL_BREAK_IF("invalid call of SymHeapCore::valDestroyTarget()");
        return;
    }

    d->destroyRoot(val);
}

int SymHeapCore::valSizeOfTarget(TValId val) const {
    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    if (valData->offRoot < 0)
        // we are above the root, so we cannot safely write anything
        return 0;

    const EValueTarget code = valData->code;
    if (isGone(code))
        return 0;

    CL_BREAK_IF(!isPossibleToDeref(valData->code));
    const TValId root = valData->valRoot;
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);

    const int rootSize = rootData->size;
    const TOffset off = valData->offRoot;
    return rootSize - off;
}

void SymHeapCore::valSetLastKnownTypeOfTarget(TValId root, TObjType clt) {
    RootValue *rootData;
    d->ents.getEntRW(&rootData, root);

    if (VAL_ADDR_OF_RET == root) {
        // destroy any stale target of VAL_ADDR_OF_RET
        d->destroyRoot(root);

        // allocate a new root value at VAL_ADDR_OF_RET
        rootData->code = VT_ON_STACK;
        rootData->size = clt->size;
    }

    // convert a type-free object into a type-aware object
    rootData->lastKnownClt = clt;
}

TObjType SymHeapCore::valLastKnownTypeOfTarget(TValId root) const {
    CL_BREAK_IF(this->valOffset(root));
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    return rootData->lastKnownClt;
}

void SymHeapCore::Private::destroyRoot(TValId root) {
    RootValue *rootData;
    this->ents.getEntRW(&rootData, root);

    EValueTarget code = VT_DELETED;
    const CVar cv = rootData->cVar;
    if (cv.uid != /* heap object */ -1) {
        // remove the corresponding program variable
        RefCntLib<RCO_NON_VIRT>::requireExclusivity(this->cVarMap);
        this->cVarMap->remove(cv);
        code = VT_LOST;
    }

    // start with the root itself as anchor
    std::vector<AnchorValue *> refs(1, rootData);

    // collect all VT_RANGE anchors
    BOOST_FOREACH(const TValId rVal, rootData->dependentValues) {
        AnchorValue *anchorData;
        this->ents.getEntRW(&anchorData, rVal);
        refs.push_back(anchorData);
    }

    BOOST_FOREACH(AnchorValue *anchorData, refs) {
        // mark the anchor value as deleted/lost
        anchorData->code = code;

        // mark all associated off-values as deleted/lost
        BOOST_FOREACH(TOffMap::const_reference item, anchorData->offMap) {
            const TValId val = item.second;
            BaseValue *valData;
            this->ents.getEntRW(&valData, val);
            valData->code = code;
        }
    }

    // release the root
    RefCntLib<RCO_NON_VIRT>::requireExclusivity(this->liveRoots);
    this->liveRoots->erase(root);

    const TOffset size = rootData->size;
    if (size) {
        // look for inner objects
        const TMemChunk chunk(0, size);
        TObjIdSet allObjs;
        if (arenaLookup(&allObjs, rootData->arena, chunk, OBJ_INVALID)) {
            // destroy all inner objects
            BOOST_FOREACH(const TObjId obj, allObjs)
                this->objDestroy(obj, /* removeVal */ true, /* detach */ false);
        }
    }

    // wipe rootData
    rootData->size = 0;
    rootData->lastKnownClt = 0;
    rootData->liveObjs.clear();
    rootData->arena.clear();
}

TValId SymHeapCore::valCreate(EValueTarget code, EValueOrigin origin) {
    switch (code) {
        case VT_UNKNOWN:
            // this is the most common case

        case VT_DELETED:
        case VT_LOST:
            // these are used by symcut
            break;

        default:
            CL_BREAK_IF("invalid call of SymHeapCore::valCreate()");
    }

    return d->valCreate(code, origin);
}

TValId SymHeapCore::valWrapCustom(CustomValue cVal) {
    ECustomValue &code = cVal.code;
    IR::TInt &num = cVal.data.num;

    switch (code) {
        case CV_INT_RANGE:
            if (!isSingular(cVal.data.rng)) {
                // CV_INT_RANGE with a valid range (do not recycle these)
                const TValId val = d->valCreate(VT_CUSTOM, VO_ASSIGNED);
                InternalCustomValue *valData;
                d->ents.getEntRW(&valData, val);
                valData->customData = cVal;
                return val;
            }

            code = CV_INT;
            num = cVal.data.rng.lo;
            // fall through!

        case CV_INT:
            // short-circuit for special integral values
            switch (num) {
                case 0:
                    return VAL_NULL;

                case 1:
                    return VAL_TRUE;

                default:
                    break;
            }

        default:
            break;
    }

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d->cValueMap);
    TValId &val = d->cValueMap->lookup(cVal);
    if (VAL_INVALID != val)
        // custom value already wrapped, we have to reuse it
        return val;

    // cVal not found, wrap it as a new heap value
    val = d->valCreate(VT_CUSTOM, VO_ASSIGNED);
    InternalCustomValue *valData;
    d->ents.getEntRW(&valData, val);
    valData->customData = cVal;
    return val;
}

const CustomValue& SymHeapCore::valUnwrapCustom(TValId val) const
{
    const InternalCustomValue *valData;
    d->ents.getEntRO(&valData, val);

    if (CV_INT_RANGE != valData->customData.code)
        // check the consistency of backward mapping
        CL_BREAK_IF(val != d->cValueMap->lookup(valData->customData));

    return valData->customData;
}

bool SymHeapCore::valTargetIsProto(TValId val) const {
    if (val <= 0)
        // not a prototype for sure
        return false;

    const BaseValue *valData;
    d->ents.getEntRO(&valData, val);
    if (!isPossibleToDeref(valData->code))
        // not a prototype for sure
        return false;

    // seek root
    const TValId root = valData->valRoot;
    const RootValue *rootData;
    d->ents.getEntRO(&rootData, root);
    return rootData->isProto;
}

void SymHeapCore::valTargetSetProto(TValId root, bool isProto) {
    CL_BREAK_IF(!isPossibleToDeref(this->valTarget(root)));
    CL_BREAK_IF(this->valOffset(root));

    RootValue *rootData;
    d->ents.getEntRW(&rootData, root);
    rootData->isProto = isProto;
}

bool SymHeapCore::proveNeq(TValId valA, TValId valB) const {
    // check for invalid values
    if (VAL_INVALID == valA || VAL_INVALID == valB)
        return false;

    // check for identical values
    if (valA == valB)
        return false;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(*this, valA, valB);

    // check for known bool values
    if (VAL_TRUE == valA)
        return (VAL_FALSE == valB);

    // we presume (0 <= valA) and (0 < valB) at this point
    CL_BREAK_IF(d->ents.outOfRange(valB));
    if (valInsideSafeRange(*this, valA) && valInsideSafeRange(*this, valB))
        // NOTE: we know (valA != valB) at this point, look above
        return true;

    // check for a Neq predicate
    if (d->neqDb->chk(valA, valB))
        return true;

    if (valA <= 0 || valB <= 0)
        // no handling of special values here
        return false;

    const TValId root1 = this->valRoot(valA);
    const TValId root2 = this->valRoot(valB);
    if (root1 == root2) {
        // same root, different offsets
        CL_BREAK_IF(matchOffsets(*this, *this, valA, valB));
        return true;
    }

    const TOffset offA = this->valOffset(valA);
    const TOffset offB = this->valOffset(valB);

    const TOffset diff = offB - offA;
    if (!diff)
        // check for Neq between the roots
        return d->neqDb->chk(root1, root2);

    SymHeapCore &writable = /* XXX */ *const_cast<SymHeapCore *>(this);
    return d->neqDb->chk(root1, writable.valByOffset(root2,  diff))
        && d->neqDb->chk(root2, writable.valByOffset(root1, -diff));
}


// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeap
struct AbstractRoot {
    RefCounter                      refCnt;

    EObjKind                        kind;
    BindingOff                      bOff;
    unsigned                        minLength;

    AbstractRoot(EObjKind kind_, BindingOff bOff_):
        kind(kind_),
        bOff(bOff_),
        minLength(0)
    {
    }

    AbstractRoot* clone() const {
        return new AbstractRoot(*this);
    }
};

struct SymHeap::Private {
    RefCounter                      refCnt;
    EntStore<AbstractRoot>          absRoots;
};

SymHeap::SymHeap(TStorRef stor, Trace::Node *trace):
    SymHeapCore(stor, trace),
    d(new Private)
{
}

SymHeap::SymHeap(const SymHeap &ref):
    SymHeapCore(ref),
    d(ref.d)
{
    RefCntLib<RCO_NON_VIRT>::enter(d);
}

SymHeap::~SymHeap() {
    RefCntLib<RCO_NON_VIRT>::leave(d);
}

SymHeap& SymHeap::operator=(const SymHeap &ref) {
    SymHeapCore::operator=(ref);

    RefCntLib<RCO_NON_VIRT>::leave(d);

    d = ref.d;
    RefCntLib<RCO_NON_VIRT>::enter(d);

    return *this;
}

void SymHeap::swap(SymHeapCore &baseRef) {
    // swap base
    SymHeapCore::swap(baseRef);

    // swap self
    SymHeap &ref = DCAST<SymHeap &>(baseRef);
    swapValues(this->d, ref.d);
}

TValId SymHeap::valClone(TValId val) {
    const TValId dup = SymHeapCore::valClone(val);
    if (dup <= 0 || VT_RANGE == this->valTarget(val))
        return dup;

    const TValId valRoot = this->valRoot(val);
    if (!d->absRoots.isValidEnt(valRoot))
        return dup;

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d);

    // clone the data
    const AbstractRoot *tplData = d->absRoots.getEntRO(valRoot);
    const TValId dupRoot = this->valRoot(dup);
    AbstractRoot *dupData = tplData->clone();
    d->absRoots.assignId(dupRoot, dupData);

    return dup;
}

EObjKind SymHeap::valTargetKind(TValId val) const {
    if (val <= 0)
        return OK_CONCRETE;

    const TValId valRoot = this->valRoot(val);
    if (!d->absRoots.isValidEnt(valRoot))
        return OK_CONCRETE;

    const AbstractRoot *aData = d->absRoots.getEntRO(valRoot);
    return aData->kind;
}

bool SymHeap::hasAbstractTarget(TValId val) const {
    return (OK_CONCRETE != this->valTargetKind(val));
}

const BindingOff& SymHeap::segBinding(TValId root) const {
    CL_BREAK_IF(this->valOffset(root));
    CL_BREAK_IF(!this->hasAbstractTarget(root));
    CL_BREAK_IF(!d->absRoots.isValidEnt(root));

    const AbstractRoot *aData = d->absRoots.getEntRO(root);
    return aData->bOff;
}

void SymHeap::valTargetSetAbstract(
        TValId                      root,
        EObjKind                    kind,
        const BindingOff            &off)
{
    CL_BREAK_IF(!isPossibleToDeref(this->valTarget(root)));
    CL_BREAK_IF(this->valOffset(root));
    CL_BREAK_IF(OK_CONCRETE == kind);

    // there is no 'prev' offset in OK_SEE_THROUGH
    CL_BREAK_IF(OK_SEE_THROUGH == kind && off.prev != off.next);

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d);

    // clone the data
    if (d->absRoots.isValidEnt(root)) {
        CL_BREAK_IF(OK_SLS != kind);

        AbstractRoot *aData = d->absRoots.getEntRW(root);
        CL_BREAK_IF(OK_SEE_THROUGH != aData->kind || off != aData->bOff);

        // OK_SEE_THROUGH -> OK_SLS
        aData->kind = kind;
        return;
    }

    AbstractRoot *aData = new AbstractRoot(kind, (OK_OBJ_OR_NULL == kind)
            ? BindingOff(OK_OBJ_OR_NULL)
            : off);

    // register a new abstract root
    d->absRoots.assignId(root, aData);
}

void SymHeap::valTargetSetConcrete(TValId root) {
    CL_DEBUG("SymHeap::objSetConcrete() is taking place...");
    CL_BREAK_IF(!isPossibleToDeref(this->valTarget(root)));
    CL_BREAK_IF(this->valOffset(root));
    CL_BREAK_IF(!d->absRoots.isValidEnt(root));

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d);

    // unregister an abstract object
    // FIXME: suboptimal code of EntStore::releaseEnt() with SH_REUSE_FREE_IDS
    d->absRoots.releaseEnt(root);
}

void SymHeap::valMerge(TValId v1, TValId v2) {
    // check that at least one value is unknown
    moveKnownValueToLeft(*this, v1, v2);
    const EValueTarget code1 = this->valTarget(v1);
    const EValueTarget code2 = this->valTarget(v2);
    CL_BREAK_IF(isKnownObject(code2));

    if (VT_ABSTRACT != code1 && VT_ABSTRACT != code2) {
        // no abstract objects involved
        SymHeapCore::valReplace(v2, v1);
        return;
    }

    if (VT_ABSTRACT == code1 && spliceOutAbstractPath(*this, v1, v2))
        // splice-out succeeded ... ls(v1, v2)
        return;

    if (VT_ABSTRACT == code2 && spliceOutAbstractPath(*this, v2, v1))
        // splice-out succeeded ... ls(v2, v1)
        return;

    CL_DEBUG("failed to splice-out list segment, has to over-approximate");
}

void SymHeap::segMinLengthOp(ENeqOp op, TValId at, unsigned len) {
    CL_BREAK_IF(!len);

    if (NEQ_DEL == op) {
        this->segSetMinLength(at, len - 1);
        return;
    }

    CL_BREAK_IF(NEQ_ADD != op);
    const unsigned current = this->segMinLength(at);
    if (len <= current)
        return;

    this->segSetMinLength(at, len);
}

bool haveSegBidir(
        TValId                      *pDst,
        const SymHeap               *sh,
        const EObjKind              kind,
        const TValId                v1,
        const TValId                v2)
{
    if (haveSeg(*sh, v1, v2, kind)) {
        *pDst = sh->valRoot(v1);
        return true;
    }

    if (haveSeg(*sh, v2, v1, kind)) {
        *pDst = sh->valRoot(v2);
        return true;
    }

    // found nothing
    return false;
}

void SymHeap::neqOp(ENeqOp op, TValId v1, TValId v2) {
    CL_BREAK_IF(NEQ_ADD != op && NEQ_DEL != op);
    CL_BREAK_IF(v1 <= 0 && v2 <= 0);

    if (!this->hasAbstractTarget(v1) && !this->hasAbstractTarget(v2)) {
        // fallback to the base implementation
        SymHeapCore::neqOp(op, v1, v2);
        return;
    }

    if (VAL_NULL == v1 && !this->valOffset(v2))
        v1 = segNextRootObj(*this, v2);
    if (VAL_NULL == v2 && !this->valOffset(v1))
        v2 = segNextRootObj(*this, v1);

    TValId seg;
    if (haveSegBidir(&seg, this, OK_SEE_THROUGH, v1, v2)
            || haveSegBidir(&seg, this, OK_OBJ_OR_NULL, v1, v2)) {
        // replace OK_SEE_THROUGH/OK_OBJ_OR_NULL by OK_CONCRETE
        this->valTargetSetConcrete(seg);
        return;
    }

    if (haveSegBidir(&seg, this, OK_SLS, v1, v2)) {
        this->segMinLengthOp(op, seg, /* SLS 1+ */ 1);
        return;
    }

    if (haveSegBidir(&seg, this, OK_DLS, v1, v2)) {
        this->segMinLengthOp(op, seg, /* DLS 1+ */ 1);
        return;
    }

    if (haveDlSegAt(*this, v1, v2)) {
        this->segMinLengthOp(op, v1, /* DLS 2+ */ 2);
        return;
    }

    CL_BREAK_IF(NEQ_ADD != op);
    CL_DEBUG("SymHeap::neqOp() refuses to add an extraordinary Neq predicate");
}

bool SymHeap::proveNeq(TValId ref, TValId val) const {
    if (SymHeapCore::proveNeq(ref, val))
        return true;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(*this, ref, val);
    if (this->hasAbstractTarget(ref) && this->hasAbstractTarget(val)) {
        const TValId seg = this->valRoot(val);
        if (objMinLength(*this, seg))
            // move the non-empty one to left
            swapValues(ref, val);
    }

    const EValueTarget refCode = this->valTarget(ref);
    if (isAbstract(refCode)) {
        // both values are abstract
        const TValId root1 = this->valRoot(ref);
        const TValId root2 = this->valRoot(val);
        if (root2 == segPeer(*this, root1)) {
            // one value points at segment and the other points at its peer
            const TOffset off1 = this->valOffset(ref);
            const TOffset off2 = this->valOffset(val);
            return (off1 == off2)
                && (1 < this->segMinLength(root1));
        }

        if (!objMinLength(*this, root1))
            // both targets are possibly empty, giving up
            return false;
    }

    std::set<TValId> haveSeen;

    EValueTarget code = this->valTarget(val);
    TOffset off /* just to silence gcc */ = -1;
    if (VT_RANGE != code)
        off = this->valOffset(val);

    while (0 < val && insertOnce(haveSeen, val)) {
        switch (code) {
            case VT_ON_STACK:
            case VT_ON_HEAP:
            case VT_STATIC:
            case VT_DELETED:
            case VT_LOST:
            case VT_CUSTOM:
                // concrete object reached --> prove done
                return (val != ref);

            case VT_RANGE:
                // TODO: improve the reasoning about VT_RANGE values
                return (VAL_NULL == ref);

            case VT_ABSTRACT:
                break;

            default:
                // we can't prove much for unknown values
                return false;
        }

        SymHeap &writable = *const_cast<SymHeap *>(this);

        TValId seg = this->valRoot(val);
        if (OK_DLS == this->valTargetKind(val))
            seg = dlSegPeer(writable, seg);

        if (seg < 0)
            // no valid object here
            return false;

        if (this->segMinLength(seg))
            // non-empty abstract object reached
            return (VAL_NULL == ref)
                || isKnownObject(refCode);

        // jump to next value while taking the 'head' offset into consideration
        const BindingOff &bOff = this->segBinding(seg);
        const TValId valNext = nextValFromSeg(writable, seg);
        val = writable.valByOffset(valNext, off - bOff.head);
        code = this->valTarget(val);
    }

    return false;
}

void SymHeap::valDestroyTarget(TValId root) {
    SymHeapCore::valDestroyTarget(root);
    if (!d->absRoots.isValidEnt(root))
        return;

    CL_DEBUG("SymHeap::valDestroyTarget() destroys an abstract object");

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d);

    // unregister an abstract object
    // FIXME: suboptimal code of EntStore::releaseEnt() with SH_REUSE_FREE_IDS
    d->absRoots.releaseEnt(root);
}

unsigned SymHeap::segMinLength(TValId seg) const {
    CL_BREAK_IF(this->valOffset(seg));
    CL_BREAK_IF(!d->absRoots.isValidEnt(seg));

    const AbstractRoot *aData = d->absRoots.getEntRO(seg);

    const EObjKind kind = aData->kind;
    switch (kind) {
        case OK_SEE_THROUGH:
        case OK_OBJ_OR_NULL:
            return 0;

        case OK_SLS:
        case OK_DLS:
            return aData->minLength;

        default:
            CL_BREAK_IF("invalid call of SymHeap::segMinLength()");
            return 0;
    }
}

void SymHeap::segSetMinLength(TValId seg, unsigned len) {
    CL_BREAK_IF(this->valOffset(seg));
    CL_BREAK_IF(!d->absRoots.isValidEnt(seg));

    RefCntLib<RCO_NON_VIRT>::requireExclusivity(d);

    AbstractRoot *aData = d->absRoots.getEntRW(seg);

    const EObjKind kind = aData->kind;
    switch (kind) {
        case OK_SEE_THROUGH:
            if (len)
                CL_BREAK_IF("OK_SEE_THROUGH is supposed to have zero minLength");
            return;

        case OK_OBJ_OR_NULL:
            if (len)
                CL_BREAK_IF("OK_OBJ_OR_NULL is supposed to have zero minLength");
            return;

        case OK_SLS:
#if SE_RESTRICT_SLS_MINLEN
            if ((SE_RESTRICT_SLS_MINLEN) < len)
                len = (SE_RESTRICT_SLS_MINLEN);
#endif
            break;

        case OK_DLS:
#if SE_RESTRICT_DLS_MINLEN
            if ((SE_RESTRICT_DLS_MINLEN) < len)
                len = (SE_RESTRICT_DLS_MINLEN);
#endif
            break;

        default:
            CL_BREAK_IF("invalid call of SymHeap::segMinLength()");
            return;
    }

    aData->minLength = len;
    if (OK_DLS != kind)
        return;

    const TValId peer = dlSegPeer(*this, seg);
    CL_BREAK_IF(peer == seg);
    CL_BREAK_IF(!d->absRoots.isValidEnt(peer));

    AbstractRoot *peerData = d->absRoots.getEntRW(peer);
    peerData->minLength = len;
}
