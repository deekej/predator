/*
 * Copyright (C) 2010 Kamil Dudka <kdudka@redhat.com>
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

#include "config_cl.h"
#include <cl/cldebug.hh>
#include <cl/clutil.hh>
#include <cl/storage.hh>

namespace {

void cltToStreamCore(std::ostream &out, const struct cl_type *clt) {
    out << "*((const struct cl_type *)"
        << static_cast<const void *>(clt)
        << ")";
    if (!clt)
        return;

    out << " (#" << clt->uid << ", code = ";
    switch (clt->code) {
        case CL_TYPE_UNKNOWN:    out << "CL_TYPE_UNKNOWN"   ; break;
        case CL_TYPE_VOID:       out << "CL_TYPE_VOID"      ; break;
        case CL_TYPE_FNC:        out << "CL_TYPE_FNC"       ; break;
        case CL_TYPE_PTR:        out << "CL_TYPE_PTR"       ; break;
        case CL_TYPE_ARRAY:      out << "CL_TYPE_ARRAY"     ; break;
        case CL_TYPE_STRUCT:     out << "CL_TYPE_STRUCT"    ; break;
        case CL_TYPE_UNION:      out << "CL_TYPE_UNION"     ; break;
        case CL_TYPE_ENUM:       out << "CL_TYPE_ENUM"      ; break;
        case CL_TYPE_INT:        out << "CL_TYPE_INT"       ; break;
        case CL_TYPE_REAL:       out << "CL_TYPE_REAL"      ; break;
        case CL_TYPE_BOOL:       out << "CL_TYPE_BOOL"      ; break;
        case CL_TYPE_CHAR:       out << "CL_TYPE_CHAR"      ; break;
        case CL_TYPE_STRING:     out << "CL_TYPE_STRING"    ; break;
    }
    out << ")";
}

} // namespace

typedef std::vector<int /* nth */> TFieldIdxChain;

class DumpCltVisitor {
    private:
        std::ostream &out_;

    public:
        DumpCltVisitor(std::ostream &out): out_(out) { }

        bool operator()(TFieldIdxChain ic, const struct cl_type_item *item)
            const
        {
            // indent regarding the current nest level
            const unsigned nestLevel = ic.size();
            const std::string indent(nestLevel << 2, ' ');
            out_ << indent;

            // print field name if any
            const char *name = item->name;
            if (name)
                out_ << "." << name << " = ";

            // print type at the current level
            const struct cl_type *clt = item->type;
            SE_BREAK_IF(!clt);
            cltToStreamCore(out_, clt);
            out_ << "\n";

            return /* continue */ true;
        }
};

void cltToStream(std::ostream &out, const struct cl_type *clt, bool oneline) {
    if (oneline) {
        cltToStreamCore(out, clt);
        return;
    }

    if (!clt) {
        out << "NULL\n";
        return;
    }

    // print type at the current level
    cltToStreamCore(out, clt);
    out << "\n";

    // go through the type recursively
    const DumpCltVisitor visitor(out);
    traverseTypeIc<TFieldIdxChain>(clt, visitor);
}

void acToStream(std::ostream &out, const struct cl_accessor *ac, bool oneline) {
    if (!ac) {
        out << "(empty)";
        if (!oneline)
            out << "\n";
    }

    for (int i = 0; ac; ac = ac->next, ++i) {
        out << i << ". ";
        const struct cl_type *clt = ac->type;

        const enum cl_accessor_e code = ac->code;
        switch (code) {
            case CL_ACCESSOR_REF:
                out << "CL_ACCESSOR_REF:";
                break;

            case CL_ACCESSOR_DEREF:
                out << "CL_ACCESSOR_DEREF:";
                break;

            case CL_ACCESSOR_ITEM: {
                const struct cl_type_item *item = clt->items + ac->data.item.id;
                out << "CL_ACCESSOR_ITEM: [+"
                    << item->offset << "]";
                const char *name = item->name;
                if (name)
                    out << " ." << name;
                out << ",";
                break;
            }

            case CL_ACCESSOR_DEREF_ARRAY:
                out << "CL_ACCESSOR_DEREF_ARRAY: ["
                    << ac->data.array.index << "],";
                break;
        }

        out << " clt = ";
        cltToStream(out, clt, oneline);
        if (oneline)
            out << "; ";
        else
            out << "\n";
    }
}

namespace {

void operandToStreamCstInt(std::ostream &str, const struct cl_operand &op) {
    const struct cl_cst &cst = op.data.cst;
    const int val = cst.data.cst_int.value;

    const enum cl_type_e code = op.type->code;
    switch (code) {
        case CL_TYPE_ENUM:
            str << "(enum XXX)" << val;
            break;

        case CL_TYPE_INT:
            str << "(int)" << val;
            break;

        case CL_TYPE_BOOL:
            str << (val ? "true" : "false");
            break;

        case CL_TYPE_PTR:
            if (!val) {
                str << "NULL";
                break;
            }
            // fall through!

        default:
#if SE_SELF_TEST
            SE_TRAP;
#endif
            break;
    }
}

void operandToStreamCst(std::ostream &str, const struct cl_operand &op) {
    const struct cl_cst &cst = op.data.cst;
    const enum cl_type_e code = cst.code;
    switch (code) {
        case CL_TYPE_INT:
            operandToStreamCstInt(str, op);
            break;

        case CL_TYPE_FNC: {
            const char *name = cst.data.cst_fnc.name;
            SE_BREAK_IF(!name);

            str << name;
            break;
        }

        case CL_TYPE_STRING: {
            const char *text = cst.data.cst_string.value;
            SE_BREAK_IF(!text);

            str << "\"" << text << "\"";
            break;
        }

        default:
#if SE_SELF_TEST
            SE_TRAP;
#endif
            break;
    }
}

const char* fieldName(const struct cl_accessor *ac) {
    SE_BREAK_IF(!ac || ac->code != CL_ACCESSOR_ITEM);

    const struct cl_type *clt = ac->type;
    SE_BREAK_IF(!clt);

    const int id = ac->data.item.id;
    SE_BREAK_IF(clt->item_cnt <= id || id < 0);

    const char *name = clt->items[id].name;
    return (name)
        ? name
        : "<anon_item>";
}

void operandToStreamAcs(std::ostream &str, const struct cl_accessor *ac) {
    if (!ac)
        return;

    // FIXME: copy/pasted from cl_pp.cc
    if (ac && ac->code == CL_ACCESSOR_DEREF &&
            ac->next && ac->next->code == CL_ACCESSOR_ITEM)
    {
        ac = ac->next;
        str << "->" << fieldName(ac);
        ac = ac->next;
    }

    for (; ac; ac = ac->next) {
        enum cl_accessor_e code = ac->code;
        switch (code) {
            case CL_ACCESSOR_DEREF_ARRAY:
                str << " [...]";
                break;

            case CL_ACCESSOR_ITEM:
                str << "." << fieldName(ac);
                break;

            case CL_ACCESSOR_REF:
                if (!ac->next)
                    // already handled
                    break;
                // fall through!

            default:
#if SE_SELF_TEST
                SE_TRAP;
#endif
                break;
        }
    }
}

void operandToStreamVar(std::ostream &str, const struct cl_operand &op) {
    const struct cl_accessor *ac = op.accessor;

    // FIXME: copy/pasted from cl_pp.cc
    const struct cl_accessor *is_ref = ac;
    while (is_ref && (is_ref->next || is_ref->code != CL_ACCESSOR_REF))
        is_ref = is_ref->next;
    if (is_ref)
        str << "&";

    if (ac && ac->code == CL_ACCESSOR_DEREF &&
            (!ac->next || ac->next->code != CL_ACCESSOR_ITEM))
    {
        str << "*";
        ac = ac->next;
    }

    // obtain var ID and name (if any)
    const char *name = NULL;
    const int uid = varIdFromOperand(&op, &name);

    // print var itself
    str << "#" << uid;
    if (name)
        str << ":" << name;

    // print all accessors excecpt CL_ACCESSOR_REF, which shloud have been
    // already handled
    operandToStreamAcs(str, ac);
}

} // namespace

void operandToStream(std::ostream &str, const struct cl_operand &op) {
    const enum cl_operand_e code = op.code;
    switch (code) {
        case CL_OPERAND_CST:
            operandToStreamCst(str, op);
            break;

        case CL_OPERAND_VAR:
            operandToStreamVar(str, op);
            break;

        case CL_OPERAND_VOID:
            // this should have been handled elsewhere
        default:
#if SE_SELF_TEST
            SE_TRAP;
#endif
            break;
    }
}

namespace {

void unOpToStream(std::ostream &str, int subCode,
                  const CodeStorage::TOperandList &opList)
{
    operandToStream(str, opList[/* dst */ 0]);
    str << " = ";

    // FIXME: copy/pasted from cl_pp.cc
    const enum cl_unop_e code = static_cast<enum cl_unop_e>(subCode);
    switch (code) {
        case CL_UNOP_ASSIGN:
            break;

        case CL_UNOP_TRUTH_NOT:     str << "!";     break;
        case CL_UNOP_BIT_NOT:       str << "~";     break;
        case CL_UNOP_MINUS:         str << "-";     break;
    }

    operandToStream(str, opList[/* src */ 1]);
}

void binOpToStream(std::ostream &str, int subCode,
                   const CodeStorage::TOperandList &opList)
{
    const enum cl_binop_e code = static_cast<enum cl_binop_e>(subCode);
    operandToStream(str, opList[/* dst */ 0]);
    str << " = (";
    operandToStream(str, opList[/* src1 */ 1]);

    // TODO: move this to cl API (or clutil)
    switch (code) {
        case CL_BINOP_EQ:               str << " == ";          break;
        case CL_BINOP_NE:               str << " != ";          break;
        case CL_BINOP_LT:               str << " < ";           break;
        case CL_BINOP_GT:               str << " > ";           break;
        case CL_BINOP_LE:               str << " <= ";          break;
        case CL_BINOP_GE:               str << " >= ";          break;
        case CL_BINOP_PLUS:             str << " + ";           break;
        case CL_BINOP_MINUS:            str << " - ";           break;
        case CL_BINOP_MULT:             str << " * ";           break;
        case CL_BINOP_TRUNC_DIV:        str << " / ";           break;
        case CL_BINOP_TRUNC_MOD:        str << " % ";           break;
        case CL_BINOP_POINTER_PLUS:     str << " (ptr +) ";     break;
        case CL_BINOP_BIT_IOR:          str << " | ";           break;
        case CL_BINOP_BIT_AND:          str << " & ";           break;
        case CL_BINOP_BIT_XOR:          str << " ^ ";           break;
        default:
#if SE_SELF_TEST
            SE_TRAP;
#endif
            break;
    }

    operandToStream(str, opList[/* src2 */ 2]);
    str << ")";
}

void callToStream(std::ostream &str, const CodeStorage::TOperandList &opList) {
    const struct cl_operand &dst = opList[/* dst */ 0];
    if (CL_OPERAND_VOID != dst.code) {
        operandToStream(str, dst);
        str << " = ";
    }
    operandToStream(str, opList[/* fnc */ 1]);
    str << " (";
    for (unsigned i = /* dst + fnc */ 2; i < opList.size(); ++i)
    {
        if (2 < i)
            str << ", ";

        operandToStream(str, opList[i]);
    }
    str << ")";
}

void retToStream(std::ostream &str, const struct cl_operand &src) {
    str << "return";

    if (CL_OPERAND_VOID == src.code)
        return;

    str << " ";
    operandToStream(str, src);
}

} // namespace

void insnToStream(std::ostream &str, const CodeStorage::Insn &insn) {
    const CodeStorage::TOperandList &opList = insn.operands;
    const CodeStorage::TTargetList &tList = insn.targets;

    const enum cl_insn_e code = insn.code;
    switch (code) {
        case CL_INSN_UNOP:
            unOpToStream(str, insn.subCode, opList);
            break;

        case CL_INSN_BINOP:
            binOpToStream(str, insn.subCode, opList);
            break;

        case CL_INSN_CALL:
            callToStream(str, opList);
            break;

        case CL_INSN_RET:
            retToStream(str, opList[/* src */ 0]);
            break;

        case CL_INSN_COND:
            str << "if (";
            operandToStream(str, opList[/* src */ 0]);
            str << ") goto " << tList[/* then label */ 0]->name();
            str <<  " else " << tList[/* else label */ 1]->name();
            break;

        case CL_INSN_JMP:
            str << "goto " << tList[/* target */ 0]->name();
            break;

        case CL_INSN_ABORT:
            str << "abort";
            break;

        default:
#if SE_SELF_TEST
            SE_TRAP;
#endif
            break;
    }
}