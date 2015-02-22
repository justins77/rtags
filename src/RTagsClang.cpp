/* This file is part of RTags.

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "RTagsClang.h"
#include "Server.h"
#include <rct/StopWatch.h>
#include "Project.h"
#include <iostream>
#include <zlib.h>
#include "Token.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#ifdef HAVE_BACKTRACE
#undef HAVE_BACKTRACE
#endif

namespace RTags {
String eatString(CXString str)
{
    const String ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

String cursorToString(CXCursor cursor, unsigned int flags)
{
    const CXCursorKind kind = clang_getCursorKind(cursor);
    String ret;
    ret.reserve(256);
    ret += eatString(clang_getCursorKindSpelling(kind));
    if (clang_isInvalid(kind))
        return ret;

    switch (RTags::cursorType(kind)) {
    case Type_Reference:
        ret += " r";
        break;
    case Type_Cursor:
        ret += " c";
        break;
    case Type_Other:
        ret += " o";
        break;
    case Type_Include:
        ret += " i";
        break;
    }

    const String name = eatString(clang_getCursorDisplayName(cursor));
    const String other = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty())
        ret += " " + name;
    if (other != name && !other.isEmpty())
        ret += " " + other;

    if (clang_isCursorDefinition(cursor))
        ret += " def";

    if (flags & IncludeUSR) {
        const String usr = eatString(clang_getCursorUSR(clang_getCanonicalCursor(cursor)));
        if (!usr.isEmpty()) {
            ret += " " + usr;
        }
    }

    if (flags & IncludeSpecializedUsr) {
        const CXCursor general = clang_getSpecializedCursorTemplate(cursor);
        if (!clang_Cursor_isNull(general)) {
            const String usr = eatString(clang_getCursorUSR(clang_getCanonicalCursor(general)));
            if (!usr.isEmpty()) {
                ret += " " + usr;
            }
        }
    }

    CXString file;
    unsigned int line, col;
    for (int pieceIndex = 0; true; ++pieceIndex) {
        CXSourceRange range = clang_Cursor_getSpellingNameRange(cursor, pieceIndex, 0);
        if (clang_Range_isNull(range))
            break;
        CXSourceLocation rangeStart = clang_getRangeStart(range);
        clang_getPresumedLocation(rangeStart, &file, &line, &col);

        const char *data = clang_getCString(file);
        if (data && *data) {
            ret += ' ';
            ret += data;
            ret += ':';
            ret += String::number(line);
            ret += ':';
            ret += String::number(col);
        }
        clang_disposeString(file);
    }
    return ret;
}

void parseTranslationUnit(const Path &sourceFile, const List<String> &args,
                          CXTranslationUnit &unit, CXIndex index,
                          CXUnsavedFile *unsaved, int unsavedCount,
                          unsigned int translationUnitFlags,
                          String *clangLine)

{
    if (clangLine)
        *clangLine = "clang ";

    int idx = 0;
    List<const char*> clangArgs(args.size() + 2, 0);

    const int count = args.size();
    for (int j=0; j<count; ++j) {
        String arg = args.at(j);
        clangArgs[idx++] = args.at(j).constData();
        if (clangLine) {
            arg.replace("\"", "\\\"");
            *clangLine += arg;
            *clangLine += ' ';
        }
    }
    // clangArgs[idx++] = "-disable-free";
    // clangArgs[idx++] = "-disable-llvm-verifier";

    if (clangLine)
        *clangLine += sourceFile;

    // StopWatch sw;
#if CINDEX_VERSION_MINOR >= 23
    for (int i=0; i<3; ++i) {
        auto error = clang_parseTranslationUnit2(index, sourceFile.constData(),
                                                 clangArgs.data(), idx, unsaved, unsavedCount,
                                                 translationUnitFlags, &unit);
        if (error != CXError_Crashed)
            break;
    }
#else
    unit = clang_parseTranslationUnit(index, sourceFile.constData(),
                                      clangArgs.data(), idx, unsaved, unsavedCount,
                                      translationUnitFlags);
#endif
    // error() << sourceFile << sw.elapsed();
}

void reparseTranslationUnit(CXTranslationUnit &unit, CXUnsavedFile *unsaved, int unsavedCount)
{
    assert(unit);
    if (clang_reparseTranslationUnit(unit, unsavedCount, unsaved, clang_defaultReparseOptions(unit)) != 0) {
        clang_disposeTranslationUnit(unit);
        unit = 0;
    }
}

struct ResolveAutoTypeRefUserData
{
    CXCursor ref;
    int index;
    Set<String> *seen;
    // List<CXCursorKind> chain;
};

static CXChildVisitResult resolveAutoTypeRefVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    ResolveAutoTypeRefUserData *userData = reinterpret_cast<ResolveAutoTypeRefUserData*>(data);
    const CXCursorKind kind = clang_getCursorKind(cursor);
    const String usr = RTags::eatString(clang_getCursorUSR(cursor));
    if (!userData->seen->insert(usr)) {
        return CXChildVisit_Break;
    }
    // userData->chain.append(kind);
    // error() << "Got here" << cursor << userData->chain;
    switch (kind) {
    case CXCursor_TypeRef:
    case CXCursor_TemplateRef:
        // error() << "Found typeRef" << cursor;
        userData->ref = cursor;
        return CXChildVisit_Break;
    case CXCursor_DeclRefExpr:
    case CXCursor_UnexposedExpr: {
        CXCursor ref = clang_getCursorReferenced(cursor);
        // error() << "got unexposed expr ref" << ref;
        switch (clang_getCursorKind(ref)) {
        case CXCursor_VarDecl:
        case CXCursor_FunctionDecl:
        case CXCursor_CXXMethod: {
            ResolveAutoTypeRefUserData u = { clang_getNullCursor(), 0, userData->seen }; //, List<CXCursorKind>() };
            clang_visitChildren(ref, resolveAutoTypeRefVisitor, &u);
            // error() << "Visited for typeRef" << u.ref
            //         << clang_isInvalid(clang_getCursorKind(u.ref))
            //         << u.chain;
            if (!clang_equalCursors(u.ref, clang_getNullCursor())) {
                userData->ref = u.ref;
                return CXChildVisit_Break;
            }
            if (userData->index + u.index > 10)
                return CXChildVisit_Break;
            break; }
        default:
            break;
        }
        break; }
    case CXCursor_ParmDecl:
        // nothing to find here
        return CXChildVisit_Break;
    default:
        break;
    }
    return CXChildVisit_Recurse;
}

CXCursor resolveAutoTypeRef(const CXCursor &cursor)
{
    assert(clang_getCursorKind(cursor) == CXCursor_VarDecl);
    Set<String> seen;
    ResolveAutoTypeRefUserData userData = { clang_getNullCursor(), 0, &seen }; //, List<CXCursorKind>() };
    clang_visitChildren(cursor, resolveAutoTypeRefVisitor, &userData);
    if (userData.index > 1) {
        if (!clang_equalCursors(userData.ref, clang_getNullCursor())) {
            // error() << "Fixed cursor for" << cursor << userData.ref;
            // << userData.chain;
            return userData.ref;
            // } else {
            //     error() << "Couldn't fix cursor for" << cursor << userData.ref;
            //             // << userData.chain;
        }
    }
    // error() << "Need to find type for" << cursor << child;
    return clang_getNullCursor();
}

static CXChildVisitResult findFirstChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    *reinterpret_cast<CXCursor*>(data) = cursor;
    return CXChildVisit_Break;
}

CXCursor findFirstChild(CXCursor parent)
{
    CXCursor ret = clang_getNullCursor();
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findFirstChildVisitor, &ret);
    return ret;
}

struct FindChildVisitor
{
    CXCursorKind kind;
    String name;
    CXCursor cursor;
};

static CXChildVisitResult findChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChildVisitor *u = reinterpret_cast<FindChildVisitor*>(data);
    if (u->name.isEmpty()) {
        if (clang_getCursorKind(cursor) == u->kind) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    } else {
        CXStringScope str = clang_getCursorSpelling(cursor);
        if (str.data() && u->name == str.data()) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    }
    return CXChildVisit_Continue;
}

CXCursor findChild(CXCursor parent, CXCursorKind kind)
{
    FindChildVisitor u = { kind, String(), clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

CXCursor findChild(CXCursor parent, const String &name)
{
    FindChildVisitor u = { CXCursor_FirstInvalid, name, clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

struct ChildrenVisitor
{
    const Filter &in;
    const Filter &out;
    List<CXCursor> children;
};

static CXChildVisitResult childrenVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    ChildrenVisitor *u = reinterpret_cast<ChildrenVisitor*>(data);
    if ((u->out.isNull() || !u->out.match(cursor)) && (u->in.isNull() || u->in.match(cursor))) {
        u->children.append(cursor);
    }
    return CXChildVisit_Continue;
}

List<CXCursor> children(CXCursor parent, const Filter &in, const Filter &out)
{
    ChildrenVisitor userData = { in, out, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, childrenVisitor, &userData);
    return userData.children;
}

struct FindChainVisitor
{
    const List<CXCursorKind> &kinds;
    List<CXCursor> ret;
};

static CXChildVisitResult findChainVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChainVisitor *u = reinterpret_cast<FindChainVisitor*>(data);
    if (clang_getCursorKind(cursor) == u->kinds.at(u->ret.size())) {
        u->ret.append(cursor);
        if (u->ret.size() < u->kinds.size())
            return CXChildVisit_Recurse;

        return CXChildVisit_Break;
    }
    return CXChildVisit_Break;
}

List<CXCursor> findChain(CXCursor parent, const List<CXCursorKind> &kinds)
{
    assert(!kinds.isEmpty());
    FindChainVisitor userData = { kinds, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChainVisitor, &userData);
    if (userData.ret.size() != kinds.size()) {
        userData.ret.clear();
    }
    return userData.ret;
}

String typeName(const CXCursor &cursor)
{
    String ret;
    switch (clang_getCursorKind(cursor)) {
    case CXCursor_FunctionTemplate:
        // ### If the return value is a template type we get an empty string here
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
        ret = typeString(clang_getResultType(clang_getCursorType(cursor)));
        break;
    case CXCursor_ClassTemplate:
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_EnumDecl:
        ret = RTags::eatString(clang_getCursorSpelling(cursor));
        break;
    case CXCursor_VarDecl: {
        const CXCursor initType = RTags::findFirstChild(cursor);
        if (clang_getCursorKind(initType) == CXCursor_InitListExpr) {
            ret = typeString(clang_getCursorType(initType));
        } else {
            ret = typeString(clang_getCursorType(cursor));
        }
        break; }
    case CXCursor_FieldDecl: // ### If the return value is a template type we get an empty string here
    case CXCursor_ParmDecl:
        ret = typeString(clang_getCursorType(cursor));
        break;
    default:
        return String();
    }
    if (!ret.isEmpty() && !ret.endsWith('*') && !ret.endsWith('&'))
        ret.append(' ');
    return ret;
}

String typeString(const CXType &type)
{
    String ret;
    if (clang_isConstQualifiedType(type))
        ret = "const ";

    const char *builtIn = builtinTypeName(type.kind);
    if (builtIn) {
        ret += builtIn;
        return ret;
    }

    if (char pointer = (type.kind == CXType_Pointer ? '*' : (type.kind == CXType_LValueReference ? '&' : 0))) {
        const CXType pointee = clang_getPointeeType(type);
        ret += typeString(pointee);
        if (ret.endsWith('*') || ret.endsWith('&')) {
            ret += pointer;
        } else {
            ret += ' ';
            ret += pointer;
        }
        return ret;
    }

    if (type.kind == CXType_ConstantArray) {
        ret += typeString(clang_getArrayElementType(type));
#if CLANG_VERSION_MAJOR > 3 || (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 1)
        const int64_t count = clang_getNumElements(type);
        ret += '[';
        if (count >= 0)
            ret += String::number(count);
        ret += ']';
#endif
        return ret;
    }
    ret += typeName(clang_getTypeDeclaration(type));
    if (ret.endsWith(' '))
        ret.chop(1);
    return ret;
}

}
