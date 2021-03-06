/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

static std::string makeUID (std::string_view name)
{
    return retainCharacters (choc::text::replace (name, " ", "_", "::", "_"),
                             "_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-");
}

bool SourceCodeModel::generate (CompileMessageList& errors, ArrayView<SourceCodeText::Ptr> filesToLoad)
{
    files.clear();
    allocator.clear();
    topLevelNamespace = AST::createRootNamespace (allocator);

    files.resize (filesToLoad.size());

    for (size_t i = 0; i < filesToLoad.size(); ++i)
    {
        auto& f = filesToLoad[i];
        auto& desc = files[i];

        try
        {
            CompileMessageHandler handler (errors);

            for (auto& m : Compiler::parseTopLevelDeclarations (allocator, f, *topLevelNamespace))
            {
                ASTUtilities::mergeDuplicateNamespaces (*topLevelNamespace);
                recurseFindingModules (m, desc);
            }
        }
        catch (AbortCompilationException) {}

        if (errors.hasErrors())
            return false;

        desc.source = f;
        desc.filename = f->filename;
        desc.UID = makeUID ("lib_" + choc::text::replace (desc.filename, ".soul", ""));
        desc.fileComment = SourceCodeUtilities::getFileSummaryComment (f);
        desc.title = SourceCodeUtilities::getFileSummaryTitle (desc.fileComment);
        desc.summary = SourceCodeUtilities::getFileSummaryBody (desc.fileComment);
    }

    buildSpecialisationParams();
    buildEndpoints();
    buildFunctions();
    buildVariables();
    buildStructs();
    buildTOCNodes();
    return true;
}

template <typename ASTType>
static std::string getFullPathForASTObject (ASTType& o)
{
    if (auto scope = o.getParentScope())
    {
        IdentifierPath parentPath;

        if (auto fn = scope->getAsFunction())
            parentPath = IdentifierPath (fn->getParentScope()->getFullyQualifiedPath(), fn->name);
        else
            parentPath = scope->getFullyQualifiedPath();

        return Program::stripRootNamespaceFromQualifiedPath (IdentifierPath (parentPath, o.name).toString());
    }

    return o.name.toString();
}

static std::string makeUID (AST::ModuleBase& m)             { return makeUID ("mod_" + Program::stripRootNamespaceFromQualifiedPath (m.getFullyQualifiedDisplayPath().toString())); }
static std::string makeUID (AST::TypeDeclarationBase& t)    { return makeUID ("type_" + getFullPathForASTObject (t)); }
static std::string makeUID (AST::VariableDeclaration& v)    { return makeUID ("var_" + getFullPathForASTObject (v)); }
static std::string makeUID (AST::EndpointDeclaration& e)    { return makeUID ("endpoint_" + getFullPathForASTObject (e)); }
static std::string makeUID (AST::Function& f)               { return makeUID ("fn_" + getFullPathForASTObject (f)); }

SourceCodeModel::ModuleDesc SourceCodeModel::createModule (AST::ModuleBase& m)
{
    SourceCodeModel::ModuleDesc d { m, allocator };

    d.UID = makeUID (m);
    d.typeOfModule = m.isNamespace() ? "namespace" : (m.isGraph() ? "graph" : "processor");
    d.fullyQualifiedName = Program::stripRootNamespaceFromQualifiedPath (m.getFullyQualifiedDisplayPath().toString());
    d.comment = SourceCodeUtilities::parseComment (SourceCodeUtilities::findStartOfPrecedingComment (m.processorKeywordLocation));

    return d;
}

void SourceCodeModel::recurseFindingModules (AST::ModuleBase& m, FileDesc& desc)
{
    if (m.originalModule != nullptr)
        return;

    // if there's no keyword then it's an outer namespace that was parsed indirectly
    if (! m.processorKeywordLocation.isEmpty())
    {
        auto module = createModule (m);

        if (shouldShow (module))
            desc.modules.push_back (std::move (module));
    }

    for (auto& sub : m.getSubModules())
        recurseFindingModules (sub, desc);
}

static SourceCodeModel::Expression operator+ (SourceCodeModel::Expression a, SourceCodeModel::Expression&& b)
{
    a.sections.reserve (a.sections.size() + b.sections.size());

    for (auto& s : b.sections)
        a.sections.push_back (std::move (s));

    return a;
}

struct ExpressionHelpers
{
    static SourceCodeModel::Expression create (AST::Expression& e)
    {
        if (auto s = cast<AST::SubscriptWithBrackets> (e))  return create (s->lhs) + createText ("[") + createIfNotNull (s->rhs) + createText ("]");
        if (auto s = cast<AST::SubscriptWithChevrons> (e))  return create (s->lhs) + createText ("<") + createIfNotNull (s->rhs) + createText (">");
        if (auto d = cast<AST::DotOperator> (e))            return create (d->lhs) + createText (".") + createText (d->rhs.identifier.toString());
        if (auto q = cast<AST::QualifiedIdentifier> (e))    return fromIdentifier (q->toString());
        if (auto c = cast<AST::Constant> (e))               return createText (c->value.getDescription());

        if (auto m = cast<AST::TypeMetaFunction> (e))
        {
            if (m->operation == AST::TypeMetaFunction::Op::makeReference)
                return create (m->source) + createText ("&");

            if (m->operation == AST::TypeMetaFunction::Op::makeConst)
                return createKeyword ("const ") + create (m->source);

            return create (m->source) + createText (".") + createText (AST::TypeMetaFunction::getNameForOperation (m->operation));
        }

        return create (e.resolveAsType());
    }

    static SourceCodeModel::Expression create (const Type& t)
    {
        if (t.isConst())          return createKeyword ("const ") + create (t.removeConst());
        if (t.isReference())      return create (t.removeReference()) + createText ("&");
        if (t.isVector())         return create (t.getPrimitiveType()) + createText ("<" + std::to_string (t.getVectorSize()) + ">");
        if (t.isUnsizedArray())   return create (t.getArrayElementType()) + createText ("[]");
        if (t.isArray())          return create (t.getArrayElementType()) + createText ("[" + std::to_string (t.getArraySize()) + "]");
        if (t.isWrapped())        return createKeyword ("wrap") + createText ("<" + std::to_string (t.getBoundedIntLimit()) + ">");
        if (t.isClamped())        return createKeyword ("clamp") + createText ("<" + std::to_string (t.getBoundedIntLimit()) + ">");
        if (t.isStruct())         return createStruct (t.getStructRef().getName());
        if (t.isStringLiteral())  return createPrimitive ("string");

        return createPrimitive (t.getPrimitiveType().getDescription());
    }

    static SourceCodeModel::Expression forVariable (AST::VariableDeclaration& v)
    {
        if (v.declaredType != nullptr)
            return create (*v.declaredType);

        SOUL_ASSERT (v.initialValue != nullptr);

        if (v.initialValue->isResolved())
            return create (v.initialValue->getResultType());

        if (auto cc = cast<AST::CallOrCast> (v.initialValue))
            return create (cc->nameOrType);

        return {};
    }

    static SourceCodeModel::Expression fromSection (SourceCodeModel::Expression::Section&& s)
    {
        SourceCodeModel::Expression d;
        d.sections.push_back (std::move (s));
        return d;
    }

    static SourceCodeModel::Expression fromIdentifier (const std::string& name)
    {
        if (name == "wrap" || name == "clamp")
             return createPrimitive (name);

        return createStruct (name);
    }

    static SourceCodeModel::Expression createIfNotNull (pool_ptr<AST::Expression> e)    { return e != nullptr ? create (*e) : SourceCodeModel::Expression(); }

    static SourceCodeModel::Expression createKeyword      (std::string s) { return fromSection ({ SourceCodeModel::Expression::Section::Type::keyword,    std::move (s) }); }
    static SourceCodeModel::Expression createText         (std::string s) { return fromSection ({ SourceCodeModel::Expression::Section::Type::text,       std::move (s) }); }
    static SourceCodeModel::Expression createPrimitive    (std::string s) { return fromSection ({ SourceCodeModel::Expression::Section::Type::primitive,  std::move (s) }); }
    static SourceCodeModel::Expression createStruct       (std::string s) { return fromSection ({ SourceCodeModel::Expression::Section::Type::structure,  std::move (s) }); }
};

std::string SourceCodeModel::Expression::toString() const
{
    std::string result;

    for (auto& s : sections)
        result += s.text;

    return result;
}

std::string SourceCodeModel::ModuleDesc::resolvePartialNameAsUID (const std::string& partialName) const
{
    AST::Scope::NameSearch search;
    search.partiallyQualifiedPath = IdentifierPath::fromString (allocator.identifiers, partialName);
    search.stopAtFirstScopeWithResults = true;
    search.findVariables = true;
    search.findTypes = true;
    search.findFunctions = true;
    search.findNamespaces = true;
    search.findProcessors = true;
    search.findProcessorInstances = false;
    search.findEndpoints = true;

    module.performFullNameSearch (search, nullptr);

    if (search.itemsFound.size() != 0)
    {
        auto item = search.itemsFound.front();

        if (auto mb = cast<AST::ModuleBase> (item))            return makeUID (*mb);
        if (auto t = cast<AST::TypeDeclarationBase> (item))    return makeUID (*t);
        if (auto v = cast<AST::VariableDeclaration> (item))    return makeUID (*v);
        if (auto e = cast<AST::EndpointDeclaration> (item))    return makeUID (*e);
        if (auto f = cast<AST::Function> (item))               return makeUID (*f);
    }

    return {};
}

SourceCodeModel::TOCNode& SourceCodeModel::TOCNode::getNode (ArrayView<std::string> path)
{
    if (path.empty())
        return *this;

    auto& firstPart = path.front();

    if (path.size() == 1 && firstPart == name)
        return *this;

    for (auto& c : children)
        if (firstPart == c.name)
            return c.getNode (path.tail());

    children.push_back ({});
    auto& n = children.back();
    n.name = firstPart;
    return path.size() > 1 ? n.getNode (path.tail()) : n;
}

bool SourceCodeModel::shouldIncludeComment (const SourceCodeUtilities::Comment& comment)
{
    return comment.isDoxygenStyle || ! comment.getText().empty();
}

SourceCodeUtilities::Comment SourceCodeModel::getComment (const AST::Context& context)
{
    return SourceCodeUtilities::parseComment (SourceCodeUtilities::findStartOfPrecedingComment (context.location.getStartOfLine()));
}

bool SourceCodeModel::shouldShow (const AST::Function& f)
{
    return shouldIncludeComment (getComment (f.context));
}

bool SourceCodeModel::shouldShow (const AST::VariableDeclaration& v)
{
    return ! v.isSpecialisation;
}

bool SourceCodeModel::shouldShow (const AST::StructDeclaration&)
{
    return true; // TODO
}

bool SourceCodeModel::shouldShow (const ModuleDesc& module)
{
    if (module.module.isProcessor())
        return true;

    if (shouldIncludeComment (module.comment))
        return true;

    if (auto functions = module.module.getFunctionList())
        for (auto& f : *functions)
            if (shouldShow (f))
                return true;

    for (auto& v : module.module.getStateVariableList())
        if (shouldShow (v))
            return true;

    for (auto& s : module.module.getStructDeclarations())
        if (shouldShow (s))
            return true;

    return false;
}

//==============================================================================
std::string SourceCodeModel::getStringBetween (CodeLocation start, CodeLocation end)
{
    SOUL_ASSERT (end.location.getAddress() >= start.location.getAddress());
    return std::string (start.location.getAddress(), end.location.getAddress());
}

CodeLocation SourceCodeModel::findNextOccurrence (CodeLocation start, char character)
{
    for (auto pos = start;; ++(pos.location))
    {
        auto c = *(pos.location);

        if (c == static_cast<decltype(c)> (character))
            return pos;

        if (c == 0)
            return {};
    }
}

CodeLocation SourceCodeModel::findEndOfExpression (CodeLocation start)
{
    while (! start.location.isEmpty())
    {
        auto c = *(start.location);

        if (c == ',' || c == ';' || c == ')' || c == '}')
            return start;

        if (c == '(')
            start = SourceCodeUtilities::findEndOfMatchingParen (start);
        else if (c == '{')
            start = SourceCodeUtilities::findEndOfMatchingBrace (start);
        else
            ++(start.location);
    }

    return {};
}

void SourceCodeModel::buildTOCNodes()
{
    for (auto& f : files)
    {
        std::vector<std::string> filePath { f.title };
        topLevelTOCNode.getNode (filePath).file = std::addressof (f);

        for (auto& m : f.modules)
        {
            TokenisedPathString path (m.fullyQualifiedName);
            auto modulePath = filePath;

            if (path.sections.size() > 1 && path.getSection(0) == "soul")
            {
                modulePath.push_back ("soul::" + path.getSection (1));
                path.sections.erase (path.sections.begin(), path.sections.begin() + 2);
            }

            for (size_t i = 0; i < path.sections.size(); ++i)
                modulePath.push_back (path.getSection (i));

            topLevelTOCNode.getNode (modulePath).module = std::addressof (m);
        }
    }
}

static std::string getInitialiserValue (CodeLocation name)
{
    auto equalsOp = SourceCodeModel::findNextOccurrence (name, '=');
    SOUL_ASSERT (! equalsOp.isEmpty());
    ++(equalsOp.location);

    auto endOfStatement = SourceCodeModel::findEndOfExpression (equalsOp);
    SOUL_ASSERT (! endOfStatement.isEmpty());

    return SourceCodeModel::getStringBetween (equalsOp, endOfStatement);
}

static std::string getInitialiserValue (AST::VariableDeclaration& v)
{
    if (v.initialValue == nullptr)
        return {};

    return getInitialiserValue (v.context.location);
}

void SourceCodeModel::buildSpecialisationParams()
{
    for (auto& f : files)
    {
        for (auto& m : f.modules)
        {
            for (auto& p : m.module.getSpecialisationParameters())
            {
                SpecialisationParameter desc;

                if (auto u = cast<AST::UsingDeclaration> (p))
                {
                    desc.type = ExpressionHelpers::createKeyword ("using");
                    desc.name = u->name.toString();

                    if (u->targetType != nullptr)
                        desc.defaultValue = getInitialiserValue (u->context.location);
                }
                else if (auto pa = cast<AST::ProcessorAliasDeclaration> (p))
                {
                    desc.type = ExpressionHelpers::createKeyword ("processor");
                    desc.name = pa->name.toString();

                    if (pa->targetProcessor != nullptr)
                        desc.defaultValue = getInitialiserValue (pa->context.location);
                }
                else if (auto na = cast<AST::NamespaceAliasDeclaration> (p))
                {
                    desc.type = ExpressionHelpers::createKeyword ("namespace");
                    desc.name = na->name.toString();

                    if (na->targetNamespace != nullptr)
                        desc.defaultValue = getInitialiserValue (na->context.location);
                }
                else if (auto v = cast<AST::VariableDeclaration> (p))
                {
                    desc.type = ExpressionHelpers::forVariable (*v);
                    desc.name = v->name.toString();
                    desc.defaultValue = getInitialiserValue (*v);
                }
                else
                {
                    SOUL_ASSERT_FALSE;
                }

                desc.UID = makeUID ("specparam_" + m.fullyQualifiedName + "_" + desc.name);
                m.specialisationParams.push_back (std::move (desc));
            }
        }
    }
}

void SourceCodeModel::buildEndpoints()
{
    for (auto& f : files)
    {
        for (auto& m : f.modules)
        {
            for (auto& e : m.module.getEndpoints())
            {
                Endpoint desc;
                desc.comment = getComment (e->context);
                desc.endpointType = endpointTypeToString (e->details->endpointType);
                desc.name = e->name.toString();
                desc.UID = makeUID (e);

                for (auto& type : e->details->dataTypes)
                    desc.dataTypes.push_back (ExpressionHelpers::create (type));

                if (e->isInput)
                    m.inputs.push_back (std::move (desc));
                else
                    m.outputs.push_back (std::move (desc));
            }
        }
    }
}

void SourceCodeModel::buildFunctions()
{
    for (auto& file : files)
    {
        for (auto& m : file.modules)
        {
            if (auto functions = m.module.getFunctionList())
            {
                for (auto& f : *functions)
                {
                    if (shouldShow (f))
                    {
                        Function desc;
                        desc.comment = getComment (f->context);
                        desc.bareName = f->name.toString();
                        desc.fullyQualifiedName = TokenisedPathString::join (m.fullyQualifiedName, desc.bareName);
                        desc.UID = makeUID (f);

                        auto openParen = findNextOccurrence (f->nameLocation.location, '(');
                        SOUL_ASSERT (! openParen.isEmpty());

                        desc.nameWithGenerics = simplifyWhitespace (getStringBetween (f->nameLocation.location, openParen));

                        if (auto ret = f->returnType.get())
                            desc.returnType = ExpressionHelpers::create (*ret);

                        for (auto& p : f->parameters)
                        {
                            Variable param;
                            param.comment = getComment (p->context);
                            param.name = p->name.toString();
                            param.UID = makeUID (p);
                            param.type = ExpressionHelpers::forVariable (p);
                            param.initialiser = getInitialiserValue (p);

                            desc.parameters.push_back (std::move (param));
                        }

                        m.functions.push_back (std::move (desc));
                    }
                }
            }
        }
    }
}

void SourceCodeModel::buildStructs()
{
    for (auto& f : files)
    {
        for (auto& m : f.modules)
        {
            for (auto& s : m.module.getStructDeclarations())
            {
                if (shouldShow (s))
                {
                    Struct desc;
                    desc.comment = getComment (s->context);
                    desc.shortName = s->name.toString();
                    desc.fullName = TokenisedPathString::join (m.fullyQualifiedName, desc.shortName);
                    desc.UID = makeUID (s);

                    for (auto& sm : s->getMembers())
                    {
                        Struct::Member member;
                        member.name = sm.name.toString();
                        member.comment = getComment (sm.nameLocation);
                        member.type = ExpressionHelpers::create (sm.type);

                        desc.members.push_back (std::move (member));
                    }

                    m.structs.push_back (std::move (desc));
                }
            }
        }
    }
}

void SourceCodeModel::buildVariables()
{
    for (auto& f : files)
    {
        for (auto& m : f.modules)
        {
            for (auto& v : m.module.getStateVariableList())
            {
                if (shouldShow (v))
                {
                    Variable desc;
                    desc.comment = getComment (v->context);
                    desc.name = v->name.toString();
                    desc.UID = makeUID (v);
                    desc.isExternal = v->isExternal;
                    desc.type = ExpressionHelpers::forVariable (v);
                    desc.initialiser = getInitialiserValue (v);

                    m.variables.push_back (std::move (desc));
                }
            }
        }
    }
}

} // namespace soul
