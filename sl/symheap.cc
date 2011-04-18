/*
 * Copyright (C) 2009-2010 Kamil Dudka <kdudka@redhat.com>
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
#include <cl/code_listener.h>

#include "symabstract.hh"
#include "symdump.hh"
#include "symseg.hh"
#include "symutil.hh"
#include "util.hh"
#include "worklist.hh"

#include <algorithm>
#include <map>
#include <set>
#include <stack>

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/tuple/tuple_comparison.hpp>

#ifndef DEBUG_UNUSED_VALUES
#   define DEBUG_UNUSED_VALUES 0
#endif

// NeqDb helper
template <class TDst>
void emitOne(TDst &dst, TValueId val) {
#if 0
    // the following condition seems to clash with our needs in SymPlot
    if (val <= 0)
        return;
#endif
    dst.push_back(val);
}

/// cluster values that are related by known offset between each other
class OffsetDb {
    public:
        typedef SymHeapCore::TOffVal                TOffVal;
        typedef SymHeapCore::TOffValCont            TOffValCont;

    private:
        typedef std::map<TOffVal, TValueId>         TOffMap;
        typedef std::map<TValueId, TOffValCont>     TValMap;
        TOffMap                 offMap_;
        TValMap                 valMap_;
        const TOffValCont       empty_;

        void addNoClobber(const TOffVal &ov, TValueId target) {
            // check for redefinition
            CL_BREAK_IF(hasKey(offMap_, ov));

            offMap_[ov] = target;
            valMap_[target].push_back(ov);
        }

    public:
        OffsetDb(): empty_() {}
        void add(TOffVal ov, TValueId target) {
            // add the given relation
            this->addNoClobber(ov, target);

            // and now the other way around
            const TValueId src = ov.first;
            ov.first = target;
            ov.second = -ov.second;
            this->addNoClobber(ov, src);
        }

        TValueId lookup(const TOffVal &ov) {
            TOffMap::iterator iter = offMap_.find(ov);
            if (offMap_.end() == iter)
                // not found
                return VAL_INVALID;

            return iter->second;
        }

        TOffset lookup(TValueId v1, TValueId v2) {
            TValMap::iterator iter = valMap_.find(v1);
            if (valMap_.end() == iter)
                return /* not found */ 0;

            BOOST_FOREACH(const TOffVal &ov, iter->second) {
                if (v2 == ov.first)
                    // FIXME: check direction
                    return ov.second;
            }

            return /* not found */ 0;
        }

        const TOffValCont& getOffValues(TValueId val) {
            TValMap::iterator iter = valMap_.find(val);
            if (valMap_.end() == iter)
                // not found
                return empty_;

            return iter->second;
        }
};

class NeqDb {
    private:
        typedef std::pair<TValueId /* valLt */, TValueId /* valGt */> TItem;
        typedef std::set<TItem> TCont;
        TCont cont_;

    public:
        bool areNeq(TValueId valLt, TValueId valGt) const {
            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            return hasKey(cont_, item);
        }
        void add(TValueId valLt, TValueId valGt) {
            CL_BREAK_IF(valLt == valGt);

            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            cont_.insert(item);
        }
        void del(TValueId valLt, TValueId valGt) {
            CL_BREAK_IF(valLt == valGt);

            sortValues(valLt, valGt);
            TItem item(valLt, valGt);
            cont_.erase(item);
        }

        bool empty() const {
            return cont_.empty();
        }

        void killByValue(TValueId val) {
            // FIXME: suboptimal due to performance
            TCont snap(cont_);
            BOOST_FOREACH(const TItem &item, snap) {
                if (item.first != val && item.second != val)
                    continue;

                CL_DEBUG("NeqDb::killByValue(#" << val
                        << ") removed dangling Neq predicate");

                cont_.erase(item);
            }
        }

        template <class TDst>
        void gatherRelatedValues(TDst &dst, TValueId val) const {
            // FIXME: suboptimal due to performance
            BOOST_FOREACH(const TItem &item, cont_) {
                if (item.first == val)
                    emitOne(dst, item.second);
                else if (item.second == val)
                    emitOne(dst, item.first);
            }
        }

        friend void SymHeapCore::copyRelevantPreds(SymHeapCore &dst,
                                                   const SymHeapCore::TValMap &)
                                                   const;

        friend bool SymHeapCore::matchPreds(const SymHeapCore &,
                                            const SymHeapCore::TValMap &)
                                            const;
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeapCore
struct SymHeapCore::Private {
    struct Object {
        TValueId            address;
        TValueId            value;

        Object(): address(VAL_INVALID), value(VAL_INVALID) { }
    };

    struct Value {
        EUnknownValue                   code;
        TObjId                          target;
        typedef std::set<TObjId>        TUsedBy;
        TUsedBy                         usedBy;

        Value(): code(UV_KNOWN), target(OBJ_INVALID) { }
    };

    std::vector<Object>     objects;
    std::vector<Value>      values;

    OffsetDb                offsetDb;
    NeqDb                   neqDb;

    void releaseValueOf(TObjId obj);
    TObjId acquireObj();
};

void SymHeapCore::Private::releaseValueOf(TObjId obj) {
    // this method is strictly private, range checks should be already done
    const TValueId val = this->objects[obj].value;
    if (val <= 0)
        return;

    Value::TUsedBy &uses = this->values.at(val).usedBy;
#ifndef NDEBUG
    if (1 != uses.erase(obj))
        // *** offset detected ***
        CL_TRAP;
#else
    uses.erase(obj);
#endif
}

TObjId SymHeapCore::Private::acquireObj() {
    const size_t last = std::max(this->objects.size(), this->values.size());
    const TObjId obj = static_cast<TObjId>(last);
    this->objects.resize(obj + 1);
    return obj;
}

SymHeapCore::SymHeapCore():
    d(new Private)
{
    d->objects.resize(/* OBJ_RETURN */ 1);
    d->values.resize(/* VAL_NULL */ 1);

    // (un)initialize OBJ_RETURN
    Private::Object &ref = d->objects[OBJ_RETURN];
    ref.value = this->valCreate(UV_UNINITIALIZED, OBJ_UNKNOWN);

    // store backward reference to OBJ_RETURN
    Private::Value &refValue = d->values[ref.value];
    refValue.usedBy.insert(OBJ_RETURN);
}

SymHeapCore::SymHeapCore(const SymHeapCore &ref):
    d(new Private(*ref.d))
{
}

SymHeapCore::~SymHeapCore() {
    delete d;
}

SymHeapCore& SymHeapCore::operator=(const SymHeapCore &ref) {
    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeapCore::swap(SymHeapCore &ref) {
    swapValues(this->d, ref.d);
}

TValueId SymHeapCore::valueOf(TObjId obj) const {
    // handle special cases first
    switch (obj) {
        case OBJ_INVALID:
            return VAL_INVALID;

        case OBJ_LOST:
        case OBJ_DELETED:
        case OBJ_DEREF_FAILED:
            return VAL_DEREF_FAILED;

        case OBJ_UNKNOWN:
            // not implemented
            CL_TRAP;

        default:
            break;
    }

    if (this->lastObjId() < obj || obj < 0)
        // object ID is either out of range, or does not represent a valid obj
        return VAL_INVALID;

    const Private::Object &ref = d->objects[obj];
    return ref.value;
}

TValueId SymHeapCore::placedAt(TObjId obj) const {
    if (this->lastObjId() < obj || obj <= 0)
        // object ID is either out of range, or does not represent a valid obj
        return VAL_INVALID;

    const Private::Object &ref = d->objects[obj];
    return ref.address;
}

TObjId SymHeapCore::pointsTo(TValueId val) const {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return OBJ_INVALID;

    const Private::Value &ref = d->values[val];
    return ref.target;
}

void SymHeapCore::usedBy(TContObj &dst, TValueId val) const {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return;

    const Private::Value::TUsedBy &usedBy = d->values[val].usedBy;
    std::copy(usedBy.begin(), usedBy.end(), std::back_inserter(dst));
}

/// return how many objects use the value
unsigned SymHeapCore::usedByCount(TValueId val) const {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return 0; // means: "not used"

    const Private::Value::TUsedBy &usedBy = d->values[val].usedBy;
    return usedBy.size();
}

TObjId SymHeapCore::objDup(TObjId objOrigin) {
    // acquire a new object
    const TObjId obj = d->acquireObj();
    d->objects[obj].address = this->valCreate(UV_KNOWN, obj);

    // copy the value inside, while keeping backward references etc.
    const Private::Object &origin = d->objects.at(objOrigin);
    SymHeapCore::objSetValue(obj, origin.value);

    // we've just created an object, let's notify posterity
    this->notifyResize(/* valOnly */ false);
    return obj;
}

TObjId SymHeapCore::objCreate() {
    // acquire object ID
    const TObjId obj = d->acquireObj();

    // obtain value pair
    const TValueId address = this->valCreate(UV_KNOWN, obj);
    const TValueId value   = this->valCreate(UV_UNINITIALIZED, OBJ_UNKNOWN);

    // keeping a reference here may cause headaches in case of reallocation
    d->objects[obj].address = address;
    d->objects[obj].value   = value;

    // store backward reference
    Private::Value &ref = d->values[value];
    ref.usedBy.insert(obj);

    // we've just created an object, let's notify posterity
    this->notifyResize(/* valOnly */ false);
    return obj;
}

void SymHeapCore::objRewriteAddress(TObjId obj, TValueId addr) {
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);
    CL_BREAK_IF(this->pointsTo(addr) < 0);

    d->objects[obj].address = addr;
}

TValueId SymHeapCore::valCreate(EUnknownValue code, TObjId target) {
    // check range (we allow OBJ_INVALID here for custom values)
    CL_BREAK_IF(this->lastObjId() < target);

    // acquire value ID
    const size_t last = std::max(d->objects.size(), d->values.size());
    const TValueId val = static_cast<TValueId>(last);
    d->values.resize(val + 1);

    Private::Value &ref = d->values[val];
    ref.code = code;
    if (UV_KNOWN == code || UV_ABSTRACT == code)
        // ignore target for unknown values, they should not be followed anyhow
        ref.target = target;

    // we've just created a new value, let's notify posterity
    this->notifyResize(/* valOnly */ true);
    return val;
}

void SymHeapCore::valSetUnknown(TValueId val, EUnknownValue code) {
#ifndef NDEBUG
    switch (code) {
        case UV_KNOWN:
        case UV_ABSTRACT:
            break;

        default:
            // please check if the caller is aware of what he's doing
            CL_TRAP;
    }
#endif

    Private::Value &ref = d->values[val];
    CL_BREAK_IF(ref.target <= 0);

    ref.code = code;
}

TObjId SymHeapCore::lastObjId() const {
    const size_t cnt = d->objects.size() - /* safe because of OBJ_RETURN */ 1;
    return static_cast<TObjId>(cnt);
}

TValueId SymHeapCore::lastValueId() const {
    const size_t cnt = d->values.size() - /* safe because of VAL_NULL */ 1;
    return static_cast<TValueId>(cnt);
}

void SymHeapCore::objSetValue(TObjId obj, TValueId val) {
    // check range
    CL_BREAK_IF(this->lastObjId()   < obj || obj < 0);
    CL_BREAK_IF(this->lastValueId() < val || val == VAL_INVALID);

    d->releaseValueOf(obj);
    d->objects[obj].value = val;
    if (val < 0)
        return;

    Private::Value &ref = d->values.at(val);
    ref.usedBy.insert(obj);
}

void SymHeapCore::pack() {
    if (!d->neqDb.empty())
        // no predicates found, we're done
        return;

    const unsigned cnt = d->values.size();
    for (unsigned i = 1; i < cnt; ++i) {
        const TValueId val = static_cast<TValueId>(i);
        if (this->usedByCount(val))
            // value used, keep going...
            continue;

        d->neqDb.killByValue(val);
    }
}

void SymHeapCore::objDestroy(TObjId obj, TObjId kind) {
#ifndef NDEBUG
    // check range (we allow to destroy OBJ_RETURN)
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);

    switch (kind) {
        case OBJ_DELETED:
        case OBJ_LOST:
            break;

        default:
            CL_TRAP;
    }
#endif

    const TValueId addr = d->objects[obj].address;
    if (0 < addr)
        d->values.at(addr).target = kind;

    d->releaseValueOf(obj);
    d->objects[obj].address = VAL_INVALID;
    d->objects[obj].value   = VAL_INVALID;
}

EUnknownValue SymHeapCore::valGetUnknown(TValueId val) const {
    switch (val) {
        case VAL_NULL: /* == VAL_FALSE */
        case VAL_TRUE:
            return UV_KNOWN;

        case VAL_INVALID:
            // fall through! (basically equal to "out of range")
        default:
            break;
    }
    CL_BREAK_IF(this->lastValueId() < val || val < 0);

    return d->values[val].code;
}

TValueId SymHeapCore::valDuplicateUnknown(TValueId val) {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return VAL_INVALID;

    const Private::Value &ref = d->values[val];
    const TValueId valNew = this->valCreate(ref.code, ref.target);

    // we've just created a new value, let's notify posterity
    this->notifyResize(/* valOnly */ true);
    return valNew;
}

/// change value of all variables with value val to (fresh) newval
void SymHeapCore::valReplace(TValueId val, TValueId newVal) {
    // collect objects having the value val
    TContObj rlist;
    this->usedBy(rlist, val);

    // go through the list and replace the value by newval
    BOOST_FOREACH(const TObjId obj, rlist) {
        this->objSetValue(obj, newVal);
    }

    // kill Neq predicate among the pair of values (if any)
    SymHeapCore::neqOp(NEQ_DEL, val, newVal);

    // reflect the change in NeqDb
    TContValue neqs;
    d->neqDb.gatherRelatedValues(neqs, val);
    BOOST_FOREACH(const TValueId neq, neqs) {
        d->neqDb.del(val, neq);
        d->neqDb.add(newVal, neq);
    }
#ifndef NDEBUG
    // make sure we didn't create any dangling predicates
    TContValue related;
    this->gatherRelatedValues(related, val);
    if (!related.empty())
        CL_TRAP;
#endif
}

// template method
bool SymHeapCore::valReplaceUnknownImpl(TValueId val, TValueId replaceBy) {
    // collect objects having the value 'val'
    TContObj rlist;
    this->usedBy(rlist, val);

    // go through the list and replace the value by 'replaceBy'
    BOOST_FOREACH(const TObjId obj, rlist) {
        this->objSetValue(obj, replaceBy);
    }

    // this implementation is so easy that it really can't fail
    return true;
}

void SymHeapCore::valReplaceUnknown(TValueId val, TValueId replaceBy) {
#ifndef NDEBUG
    // ensure there hasn't been any inequality defined among the pair
    if (d->neqDb.areNeq(val, replaceBy)) {
        CL_ERROR("inconsistency detected among values #" << val
                << " and #" << replaceBy);
        CL_TRAP;
    }
#endif
    // make it possible to override the implementation (template method)
    if (!this->valReplaceUnknownImpl(val, replaceBy)) {
        CL_WARN("overridden implementation valReplaceUnknownImpl() failed"
                ", has to over-approximate...");
#ifndef NDEBUG
        CL_NOTE("attempt to plot heap...");
        dump_plot(*this, "valReplaceUnknownImpl-failed");
#endif
    }
}

TValueId SymHeapCore::valCreateByOffset(TOffVal ov) {
    // first look if such a value already exists
    TValueId val = d->offsetDb.lookup(ov);
    if (0 < val)
        return val;

    // create a new unknown value and associate it with the offset
    val = this->valCreate(UV_UNKNOWN, /* no valid target */ OBJ_INVALID);
    d->offsetDb.add(ov, val);

    // we've just created a new value, let's notify posterity
    this->notifyResize(/* valOnly */ true);
    return val;
}

TValueId SymHeapCore::valGetByOffset(TOffVal ov) const {
    return d->offsetDb.lookup(ov);
}

void SymHeapCore::gatherOffValues(TOffValCont &dst, TValueId ref) const {
    dst = d->offsetDb.getOffValues(ref);
}

void SymHeapCore::neqOp(ENeqOp op, TValueId valA, TValueId valB) {
    switch (op) {
        case NEQ_NOP:
            return;

        case NEQ_ADD:
            d->neqDb.add(valA, valB);
            return;

        case NEQ_DEL:
            d->neqDb.del(valA, valB);
            return;
    }
}

namespace {
    void moveKnownValueToLeft(const SymHeapCore &sh,
                              TValueId &valA, TValueId &valB)
    {
        sortValues(valA, valB);

        if ((0 < valA) && UV_KNOWN != sh.valGetUnknown(valA)) {
            const TValueId tmp = valA;
            valA = valB;
            valB = tmp;
        }
    }
}

bool SymHeapCore::proveNeq(TValueId valA, TValueId valB) const {
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
    CL_BREAK_IF(this->lastValueId() < valB || valB < 0);
    const EUnknownValue code = this->valGetUnknown(valB);
    if (UV_KNOWN == code)
        // NOTE: we know (valA != valB) at this point, look above
        return true;

    // check for a Neq predicate
    if (d->neqDb.areNeq(valA, valB))
        return true;

    if (d->offsetDb.lookup(valA, valB))
        // there is an offset defined among the values, they can't be equal
        return true;

    // cannot prove non-equality
    return false;
}

void SymHeapCore::gatherRelatedValues(TContValue &dst, TValueId val) const {
    // TODO: should we care about off-values here?
    d->neqDb.gatherRelatedValues(dst, val);
}

template <class TValMap>
bool valMapLookup(const TValMap &valMap, TValueId *pVal) {
    if (*pVal <= VAL_NULL)
        // special values always match, no need for mapping
        return true;

    typename TValMap::const_iterator iter = valMap.find(*pVal);
    if (valMap.end() == iter)
        // mapping not found
        return false;

    // match
    *pVal = iter->second;
    return true;
}

void SymHeapCore::copyRelevantPreds(SymHeapCore &dst, const TValMap &valMap)
    const
{
    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb.cont_) {
        TValueId valLt = item.first;
        TValueId valGt = item.second;

        if (!valMapLookup(valMap, &valLt) || !valMapLookup(valMap, &valGt))
            // not relevant
            continue;

        // create the image now!
        dst.neqOp(NEQ_ADD, valLt, valGt);
    }
}

bool SymHeapCore::matchPreds(const SymHeapCore &ref, const TValMap &valMap)
    const
{
    // go through NeqDb
    BOOST_FOREACH(const NeqDb::TItem &item, d->neqDb.cont_) {
        TValueId valLt = item.first;
        TValueId valGt = item.second;
        if (!valMapLookup(valMap, &valLt) || !valMapLookup(valMap, &valGt))
            // seems like a dangling predicate, which we are not interested in
            continue;

        if (!ref.d->neqDb.areNeq(valLt, valGt))
            // Neq predicate not matched
            return false;
    }

    return true;
}

// /////////////////////////////////////////////////////////////////////////////
// CVar lookup container
class CVarMap {
    private:
        typedef std::map<CVar, TObjId>              TCont;
        TCont                                       cont_;

    public:
        void insert(CVar cVar, TObjId obj) {
#ifndef NDEBUG
            const unsigned last = cont_.size();
#endif
            cont_[cVar] = obj;

            // check for mapping redefinition
            CL_BREAK_IF(last == cont_.size());
        }

        void remove(CVar cVar) {
#ifndef NDEBUG
            if (1 != cont_.erase(cVar))
                // *** offset detected ***
                CL_TRAP;
#else
            cont_.erase(cVar);
#endif
        }

        TObjId find(const CVar &cVar) {
            // regular lookup
            TCont::iterator iter = cont_.find(cVar);
            const bool found = (cont_.end() != iter);
            if (!cVar.inst) {
                // gl variable explicitly requested
                return (found)
                    ? iter->second
                    : OBJ_INVALID;
            }

            // automatic fallback to gl variable
            CVar gl = cVar;
            gl.inst = /* global variable */ 0;
            TCont::iterator iterGl = cont_.find(gl);
            const bool foundGl = (cont_.end() != iterGl);

            if (!found && !foundGl)
                // not found anywhere
                return OBJ_INVALID;

            // check for clash on uid among lc/gl variable
            CL_BREAK_IF(found && foundGl);

            if (found)
                return iter->second;
            else /* if (foundGl) */
                return iterGl->second;
        }

        template <class TDst>
        void getAll(TDst &dst) {
            BOOST_FOREACH(const TCont::value_type &item, cont_) {
                dst.push_back(item.first);
            }
        }

        template <class TFunctor>
        void goThroughObjs(TFunctor &f)
        {
            BOOST_FOREACH(const TCont::value_type &item, cont_) {
                f(item.second);
            }
        }
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeapTyped
struct SymHeapTyped::Private {
    struct Object {
        const struct cl_type        *clt;
        size_t                      cbSize;
        CVar                        cVar;
        int                         nthItem; // -1  OR  0 .. parent.item_cnt-1
        TObjId                      parent;
        TContObj                    subObjs;
        bool                        isProto;

        Object():
            clt(0),
            cbSize(0),
            nthItem(-1),
            parent(OBJ_INVALID),
            isProto(false)
        {
        }
    };

    struct Value {
        const struct cl_type        *clt;
        TObjId                      compObj;
        bool                        isCustom;
        int                         customData;

        Value(): clt(0), compObj(OBJ_INVALID), isCustom(false) { }
    };

    CVarMap                         cVarMap;

    typedef std::map<int, TValueId> TCValueMap;
    TCValueMap                      cValueMap;

    std::vector<Object>             objects;
    std::vector<Value>              values;
    TContObj                        roots;
};

void SymHeapTyped::notifyResize(bool valOnly) {
    const size_t lastValueId = this->lastValueId();
    if (d->values.size() <= lastValueId)
        d->values.resize(lastValueId + 1);

    if (valOnly)
        // no objects created recently
        return;

    const size_t lastObjId = this->lastObjId();
    if (d->objects.size() <= lastObjId)
        d->objects.resize(lastObjId + 1);
}

TValueId SymHeapTyped::createCompValue(const struct cl_type *clt, TObjId obj) {
    const TValueId val = SymHeapCore::valCreate(UV_KNOWN, OBJ_INVALID);
    CL_BREAK_IF(VAL_INVALID == val);

    Private::Value &ref = d->values[val];
    ref.clt         = clt;
    ref.compObj     = obj;

    return val;
}

void SymHeapTyped::initValClt(TObjId obj) {
    // look for object's address
    const TValueId addr = SymHeapCore::placedAt(obj);
    CL_BREAK_IF(VAL_INVALID == addr);

    // initialize value's type of the address
    const struct cl_type *clt = d->objects[obj].clt;
    d->values.at(addr).clt = clt;
    if (!clt)
        // not type-info here
        return;

    const TValueId val = this->valueOf(obj);
    if (val <= 0)
        // special value inside
        return;

    Private::Value &ref = d->values.at(val);
    if (ref.clt)
        // value type already defined
        return;

    // initialize value's type of the value _inside_
    ref.clt = (CL_TYPE_PTR == clt->code)
        ? targetTypeOfPtr(clt)
        : clt;
}

TObjId SymHeapTyped::createSubVar(const struct cl_type *clt, TObjId parent) {
    const TObjId obj = SymHeapCore::objCreate();
    CL_BREAK_IF(OBJ_INVALID == obj);

    Private::Object &ref = d->objects[obj];
    ref.clt         = clt;
    ref.parent      = parent;

    this->initValClt(obj);
    return obj;
}

void SymHeapTyped::createSubs(TObjId obj) {
    const struct cl_type *clt = d->objects.at(obj).clt;

    typedef std::pair<TObjId, const struct cl_type *> TPair;
    typedef std::stack<TPair> TStack;
    TStack todo;

    // we use explicit stack to avoid recursion
    push(todo, obj, clt);
    while (!todo.empty()) {
        boost::tie(obj, clt) = todo.top();
        todo.pop();
        CL_BREAK_IF(!clt);

        if (!isComposite(clt))
            continue;

        const int cnt = clt->item_cnt;
        SymHeapCore::objSetValue(obj, this->createCompValue(clt, obj));

        // keeping a reference at this point may cause headaches in case
        // of reallocation
        d->objects[obj].subObjs.resize(cnt);
        for (int i = 0; i < cnt; ++i) {
            const struct cl_type_item *item = clt->items + i;
            const struct cl_type *subClt = item->type;
            const TObjId subObj = this->createSubVar(subClt, obj);
            d->objects[subObj].nthItem = i; // position in struct
            d->objects[obj].subObjs[i] = subObj;

            if (!item->offset && OBJ_RETURN != obj) {
                // declare implicit aliasing with parent object's addr
                SymHeapCore::objRewriteAddress(subObj, this->placedAt(obj));
            }

            push(todo, subObj, subClt);
        }
    }
}

struct ObjDupStackItem {
    TObjId  srcObj;
    TObjId  dstParent;
    int     nth;
};
TObjId SymHeapTyped::objDup(TObjId obj) {
    CL_DEBUG("SymHeapTyped::objDup() is taking place...");
    TObjId image = OBJ_INVALID;

    ObjDupStackItem item;
    item.srcObj = obj;
    item.dstParent = OBJ_INVALID;

    std::stack<ObjDupStackItem> todo;
    todo.push(item);
    while (!todo.empty()) {
        item = todo.top();
        todo.pop();

        // duplicate a single object
        const TObjId src = item.srcObj;
        const TObjId dst = SymHeapCore::objDup(src);
        if (OBJ_INVALID == image)
            image = dst;

        // copy the metadata
        d->objects[dst] = d->objects[src];
        d->objects[dst].parent = item.dstParent;

        // initialize clt of its address
        this->initValClt(dst);

        // update the reference to self in the parent object
        const TObjId parent = item.dstParent;
        if (OBJ_INVALID != parent) {
            Private::Object &refParent = d->objects.at(parent);
            refParent.subObjs[item.nth] = dst;

            if (!subOffsetIn(*this, parent, dst)) {
                // declare implicit aliasing with parent object's addr
                SymHeapCore::objRewriteAddress(dst, this->placedAt(parent));
            }
        }

        const TContObj subObjs(d->objects[src].subObjs);
        if (subObjs.empty())
            continue;

        // assume composite object
        const struct cl_type *clt = d->objects[src].clt;
        SymHeapCore::objSetValue(dst, this->createCompValue(clt, dst));

        // traverse composite types recursively
        for (unsigned i = 0; i < subObjs.size(); ++i) {
            item.srcObj     = subObjs[i];
            item.dstParent  = dst;
            item.nth        = i;
            todo.push(item);
        }
    }

    const Private::Object &ref = d->objects[obj];
    if (ref.clt && ref.clt->code == CL_TYPE_STRUCT && -1 == ref.parent)
        // if the original was a root object, the new one must also be
        // FIXME: should we care about CL_TYPE_UNION here?
        d->roots.push_back(image);

    return image;
}

void SymHeapTyped::objDestroyPriv(TObjId root) {
    typedef std::stack<TObjId> TStack;
    TStack todo;

    // we are using explicit stack to avoid recursion
    todo.push(root);
    while (!todo.empty()) {
        const TObjId obj = todo.top();
        todo.pop();

        // schedule all subvars for removal
        const Private::Object &ref = d->objects.at(obj);
        BOOST_FOREACH(TObjId subObj, ref.subObjs) {
            todo.push(subObj);
        }

        // remove current
        const TObjId kind = (-1 == ref.cVar.uid)
            ? OBJ_DELETED
            : OBJ_LOST;
        SymHeapCore::objDestroy(obj, kind);
    }

    // remove self from roots (if ever there)
    TContObj::iterator rend(
            remove_if(d->roots.begin(), d->roots.end(),
            bind2nd(std::equal_to<TObjId>(), root)));
    d->roots.erase(rend, d->roots.end());
}

SymHeapTyped::SymHeapTyped():
    d(new Private)
{
    SymHeapTyped::notifyResize(/* valOnly */ false);
}

SymHeapTyped::SymHeapTyped(const SymHeapTyped &ref):
    SymHeapCore(ref),
    d(new Private(*ref.d))
{
}

SymHeapTyped::~SymHeapTyped() {
    delete d;
}

SymHeapTyped& SymHeapTyped::operator=(const SymHeapTyped &ref) {
    SymHeapCore::operator=(ref);
    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeapTyped::swap(SymHeapCore &baseRef) {
    // swap base
    SymHeapCore::swap(baseRef);

    // swap self
    SymHeapTyped &ref = dynamic_cast<SymHeapTyped &>(baseRef);
    swapValues(this->d, ref.d);
}

void SymHeapTyped::objSetValue(TObjId obj, TValueId val) {
#ifndef NDEBUG
    // range check
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);

    if (!d->objects[obj].subObjs.empty())
        // invalid call of SymHeapTyped::objSetValue(), you want probably go
        // through SymProc::objSetValue()
        CL_TRAP;
#endif
    SymHeapCore::objSetValue(obj, val);
}

const struct cl_type* SymHeapTyped::objType(TObjId obj) const {
    if (this->lastObjId() < obj || obj < 0)
        // object ID is either out of range, or does not represent a valid obj
        // (we allow OBJ_RETURN here)
        return 0;

    return d->objects[obj].clt;
}

const struct cl_type* SymHeapTyped::valType(TValueId val) const {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point a valid obj
        return 0;

    return d->values[val].clt;
}

TValueId SymHeapTyped::valDuplicateUnknown(TValueId val) {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return VAL_INVALID;

    // duplicate the value by core
    const TValueId dup = SymHeapCore::valDuplicateUnknown(val);

    // duplicate also the type-info
    d->values.at(dup).clt = this->valType(val);

    return dup;
}

bool SymHeapTyped::cVar(CVar *dst, TObjId obj) const {
    if (this->lastObjId() < obj || obj <= 0)
        // object ID is either out of range, or does not represent a valid obj
        return false;

    const CVar &cVar = d->objects[obj].cVar;
    if (-1 == cVar.uid)
        // looks like a heap object
        return false;

    if (dst)
        // return its identification if requested to do so
        *dst = cVar;

    // non-heap object
    return true;
}

TObjId SymHeapTyped::objByCVar(CVar cVar) const {
    return d->cVarMap.find(cVar);
}

void SymHeapTyped::gatherCVars(TContCVar &dst) const {
    d->cVarMap.getAll(dst);
}

void SymHeapTyped::gatherRootObjs(TContObj &dst) const {
    dst = d->roots;
}

TObjId SymHeapTyped::valGetCompositeObj(TValueId val) const {
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point a valid obj
        return OBJ_INVALID;

    return d->values[val].compObj;
}

TObjId SymHeapTyped::subObj(TObjId obj, int nth) const {
    if (this->lastObjId() < obj || obj < 0)
        // object ID is either out of range, or does not represent a valid obj
        // (we allow OBJ_RETURN here)
        return OBJ_INVALID;

    const Private::Object &ref = d->objects[obj];
    const TContObj &subs = ref.subObjs;
    const int cnt = subs.size();

    // this helps to avoid a warning when compiling with DEBUG_SYMID_FORCE_INT
    static const TObjId OUT_OF_RANGE = OBJ_INVALID;

    return (nth < cnt)
        ? subs[nth]
        : /* nth is out of range */ OUT_OF_RANGE;
}

TObjId SymHeapTyped::objParent(TObjId obj, int *nth) const {
    if (this->lastObjId() < obj || obj <= 0)
        // object ID is either out of range, or does not represent a valid obj
        return OBJ_INVALID;

    const Private::Object &ref = d->objects[obj];
    const TObjId parent = ref.parent;
    if (OBJ_INVALID == parent)
        return OBJ_INVALID;

    if (nth)
        *nth = ref.nthItem;

    return parent;
}

TObjId SymHeapTyped::objCreate(const struct cl_type *clt, CVar cVar) {
    const TObjId obj = SymHeapCore::objCreate();
    if (OBJ_INVALID == obj)
        return OBJ_INVALID;

    Private::Object &ref = d->objects[obj];
    ref.clt     = clt;
    ref.cVar    = cVar;
    if (clt) {
        this->createSubs(obj);
        if (CL_TYPE_STRUCT == clt->code)
            // FIXME: should we care about CL_TYPE_UNION here?
            d->roots.push_back(obj);
    }

    if (/* heap object */ -1 != cVar.uid)
        d->cVarMap.insert(cVar, obj);

    this->initValClt(obj);
    return obj;
}

TValueId SymHeapTyped::heapAlloc(int cbSize) {
    const TObjId obj = SymHeapCore::objCreate();
    d->objects[obj].cbSize = cbSize;

    return this->placedAt(obj);
}

int SymHeapTyped::objSizeOfAnon(TObjId obj) const {
    CL_BREAK_IF(this->lastObjId() < obj || obj <= 0);
    const Private::Object &ref = d->objects[obj];

    // if we know the type, it's not an anonymous object
    CL_BREAK_IF(ref.clt);

    return ref.cbSize;
}

void SymHeapTyped::objDefineType(TObjId obj, const struct cl_type *clt) {
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);
    Private::Object &ref = d->objects[obj];

    // type redefinition not allowed for now
    CL_BREAK_IF(ref.clt);

    // delayed object's type definition
    ref.clt = clt;
    this->createSubs(obj);
    if (CL_TYPE_STRUCT == clt->code)
        // FIXME: should we care about CL_TYPE_UNION here?
        d->roots.push_back(obj);

    if (OBJ_RETURN == obj)
        // OBJ_RETURN has no address
        return;

    // delayed value's type definition
    this->initValClt(obj);
}

void SymHeapTyped::objDestroy(TObjId obj) {
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);
    Private::Object &ref = d->objects[obj];

    const CVar cv = ref.cVar;
    if (cv.uid != /* heap object */ -1)
        d->cVarMap.remove(cv);

    CL_BREAK_IF(OBJ_INVALID != this->objParent(obj));
    this->objDestroyPriv(obj);

    if (OBJ_RETURN == obj) {
        // (un)initialize OBJ_RETURN for next wheel
        const TValueId uv = this->valCreate(UV_UNINITIALIZED, OBJ_UNKNOWN);
        SymHeapCore::objSetValue(OBJ_RETURN, uv);

        Private::Object &ref = d->objects[OBJ_RETURN];
        ref.clt = 0;
        ref.subObjs.clear();
    }
}

TValueId SymHeapTyped::valCreateUnknown(EUnknownValue code,
                                   const struct cl_type *clt)
{
    const TValueId val = SymHeapCore::valCreate(code, OBJ_UNKNOWN);
    if (VAL_INVALID == val)
        return VAL_INVALID;

    d->values[val].clt = clt;
    return val;
}

TValueId SymHeapTyped::valCreateCustom(const struct cl_type *clt, int cVal) {
    Private::TCValueMap::iterator iter = d->cValueMap.find(cVal);
    if (d->cValueMap.end() == iter) {
        // cVal not found, create a new wrapper for it
        const TValueId val = SymHeapCore::valCreate(UV_KNOWN, OBJ_INVALID);
        if (VAL_INVALID == val)
            return VAL_INVALID;

        // initialize heap value
        Private::Value &ref = d->values[val];
        ref.clt         = clt;
        ref.isCustom    = true;
        ref.customData  = cVal;

        // store cVal --> val mapping
        d->cValueMap[cVal] = val;

        return val;
    }

    // custom value already wrapped, we have to reuse it
    const TValueId val = iter->second;

#ifndef NDEBUG
    // check heap integrity
    const Private::Value &ref = d->values.at(val);
    CL_BREAK_IF(!ref.isCustom);

    // type-info has to match
    CL_BREAK_IF(ref.clt != clt);
#endif

    return val;
}

int SymHeapTyped::valGetCustom(const struct cl_type **pClt, TValueId val) const
{
    if (this->lastValueId() < val || val <= 0)
        // value ID is either out of range, or does not point to a valid obj
        return -1;

    const Private::Value &ref = d->values[val];
    if (!ref.isCustom)
        // not a custom value
        return -1;

    if (pClt)
        // provide type info if requested to do so
        *pClt = ref.clt;

    return ref.customData;
}

bool SymHeapTyped::objIsProto(TObjId obj) const {
    if (obj <= 0)
        // not a prototype for sure
        return false;

    // jump to root
    obj = objRoot(*this, obj);

    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);
    return d->objects[obj].isProto;
}

void SymHeapTyped::objSetProto(TObjId obj, bool isProto) {
    CL_BREAK_IF(this->lastObjId() < obj || obj < 0);
    Private::Object &ref = d->objects[obj];

    // this method is supposed to be called only on root objects
    CL_BREAK_IF(-1 != ref.parent);

    ref.isProto = isProto;
}

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymHeap
struct SymHeap::Private {
    struct ObjectEx {
        EObjKind            kind;
        BindingOff          off;

        ObjectEx(): kind(OK_CONCRETE) { }
    };

    typedef std::map<TObjId, ObjectEx> TObjMap;
    TObjMap objMap;
};

SymHeap::SymHeap():
    d(new Private)
{
}

SymHeap::SymHeap(const SymHeap &ref):
    SymHeapTyped(ref),
    d(new Private(*ref.d))
{
}

SymHeap::~SymHeap() {
    delete d;
}

SymHeap& SymHeap::operator=(const SymHeap &ref) {
    SymHeapTyped::operator=(ref);
    delete d;
    d = new Private(*ref.d);
    return *this;
}

void SymHeap::swap(SymHeapCore &baseRef) {
    // swap base
    SymHeapTyped::swap(baseRef);

    // swap self
    SymHeap &ref = dynamic_cast<SymHeap &>(baseRef);
    swapValues(this->d, ref.d);
}

TObjId SymHeap::objDup(TObjId objOld) {
    const TObjId objNew = SymHeapTyped::objDup(objOld);
    Private::TObjMap::iterator iter = d->objMap.find(objOld);
    if (d->objMap.end() != iter) {
        // duplicate metadata of an abstract object
        Private::ObjectEx tmp(iter->second);
        d->objMap[objNew] = tmp;

        // set the pointing value's code to UV_ABSTRACT
        const TValueId addrNew = this->placedAt(objNew);
        SymHeapCore::valSetUnknown(addrNew, UV_ABSTRACT);
    }

    return objNew;
}

EObjKind SymHeap::objKind(TObjId obj) const {
    Private::TObjMap::iterator iter = d->objMap.find(obj);
    if (d->objMap.end() != iter)
        return iter->second.kind;

    const TObjId root = objRoot(*this, obj);
    if (!hasKey(d->objMap, root))
        return OK_CONCRETE;

    return (segHead(*this, root) == obj)
        ? OK_HEAD
        : OK_PART;
}

EUnknownValue SymHeap::valGetUnknown(TValueId val) const {
    const TObjId target = this->pointsTo(val);
    if (0 < target) {
        const EObjKind kind = this->objKind(target);
        switch (kind) {
            case OK_CONCRETE:
                break;

            default:
                return UV_ABSTRACT;
        }
    }

    return SymHeapCore::valGetUnknown(val);
}

const BindingOff& SymHeap::objBinding(TObjId obj) const {
    const TObjId root = objRoot(*this, obj);

    Private::TObjMap::iterator iter = d->objMap.find(root);
    CL_BREAK_IF(d->objMap.end() == iter);

    return iter->second.off;
}

void SymHeap::objSetAbstract(TObjId obj, EObjKind kind, const BindingOff &off)
{
    if (OK_SLS == kind && hasKey(d->objMap, obj)) {
        Private::ObjectEx &ref = d->objMap[obj];
        CL_BREAK_IF(OK_MAY_EXIST != ref.kind || off != ref.off);

        // OK_MAY_EXIST -> OK_SLS
        ref.kind = kind;
        return;
    }

    CL_BREAK_IF(OK_CONCRETE == kind || hasKey(d->objMap, obj));

    // initialize abstract object
    Private::ObjectEx &ref = d->objMap[obj];
    ref.kind    = kind;
    ref.off     = off;

    // mark the value as UV_ABSTRACT
    const TValueId addr = this->placedAt(obj);
    SymHeapCore::valSetUnknown(addr, UV_ABSTRACT);
#ifndef NDEBUG
    // check for self-loops
    const TObjId objBind = ptrObjByOffset(*this, obj, off.next);
    const TValueId valNext = this->valueOf(objBind);
    const TObjId head = compObjByOffset(*this, obj, off.head);
    CL_BREAK_IF(addr == valNext || this->placedAt(head) == valNext);
#endif
}

void SymHeap::objSetConcrete(TObjId obj) {
    CL_DEBUG("SymHeap::objSetConcrete() is taking place...");
    Private::TObjMap::iterator iter = d->objMap.find(obj);
    CL_BREAK_IF(d->objMap.end() == iter);

    // mark the address of 'head' as UV_KNOWN
    const TValueId addrHead = segHeadAddr(*this, obj);
    SymHeapCore::valSetUnknown(addrHead, UV_KNOWN);

    // mark the value as UV_KNOWN
    const TValueId addr = this->placedAt(obj);
    SymHeapCore::valSetUnknown(addr, UV_KNOWN);

    // just remove the object ID from the map
    d->objMap.erase(iter);
}

bool SymHeap::valReplaceUnknownImpl(TValueId val, TValueId replaceBy) {
    const EUnknownValue code = this->valGetUnknown(val);
    switch (code) {
        case UV_KNOWN:
            // known values are not supposed to be replaced
            return false;

        case UV_ABSTRACT:
            return spliceOutListSegment(*this, val, replaceBy);

        default:
            return SymHeapTyped::valReplaceUnknownImpl(val, replaceBy);
    }
}

void SymHeap::dlSegCrossNeqOp(ENeqOp op, TValueId headAddr1) {
    const TObjId head1 = this->pointsTo(headAddr1);
    const TObjId seg1 = objRoot(*this, head1);
    const TObjId seg2 = dlSegPeer(*this, seg1);
    const TValueId headAddr2 = segHeadAddr(*this, seg2);

    // dig pointer-to-next objects
    const TObjId next1 = nextPtrFromSeg(*this, seg1);
    const TObjId next2 = nextPtrFromSeg(*this, seg2);

    // read the values (addresses of the surround)
    const TValueId val1 = this->valueOf(next1);
    const TValueId val2 = this->valueOf(next2);

    // add/del Neq predicates
    SymHeapCore::neqOp(op, val1, headAddr2);
    SymHeapCore::neqOp(op, val2, headAddr1);

    if (NEQ_DEL == op)
        // removing the 1+ flag implies removal of the 2+ flag
        SymHeapCore::neqOp(NEQ_DEL, headAddr1, headAddr2);
}

void SymHeap::neqOp(ENeqOp op, TValueId valA, TValueId valB) {
    if (NEQ_ADD == op && haveDlSegAt(*this, valA, valB)) {
        // adding the 2+ flag implies adding of the 1+ flag
        this->dlSegCrossNeqOp(op, valA);
    }
    else {
        if (haveSeg(*this, valA, valB, OK_DLS)) {
            this->dlSegCrossNeqOp(op, valA);
            return;
        }

        if (haveSeg(*this, valB, valA, OK_DLS)) {
            this->dlSegCrossNeqOp(op, valB);
            return;
        }
    }

    SymHeapTyped::neqOp(op, valA, valB);
}

bool SymHeap::proveNeq(TValueId ref, TValueId val) const {
    if (SymHeapTyped::proveNeq(ref, val))
        return true;

    // having the values always in the same order leads to simpler code
    moveKnownValueToLeft(*this, ref, val);
    if (UV_KNOWN != this->valGetUnknown(ref))
        return false;

    std::set<TValueId> haveSeen;

    while (0 < val && insertOnce(haveSeen, val)) {
        const EUnknownValue code = this->valGetUnknown(val);
        switch (code) {
            case UV_KNOWN:
                // concrete object reached --> prove done
                return (val != ref);

            case UV_ABSTRACT:
                break;

            default:
                // we can't prove much for unknown values
                return false;
        }

        if (SymHeapCore::proveNeq(ref, val))
            // prove done
            return true;

        TObjId seg = objRootByVal(*this, val);
        if (OK_DLS == this->objKind(seg))
            seg = dlSegPeer(*this, seg);

        if (seg < 0)
            // no valid object here
            return false;

        const TValueId valNext = this->valueOf(nextPtrFromSeg(*this, seg));
        if (SymHeapCore::proveNeq(val, valNext))
            // non-empty abstract object reached --> prove done
            return true;

        // jump to next value
        val = valNext;
    }

    return false;
}

void SymHeap::objDestroy(TObjId obj) {
    SymHeapTyped::objDestroy(obj);
    d->objMap.erase(obj);
}
