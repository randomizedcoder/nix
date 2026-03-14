#include "DanglingCStrCheck.h"

#include <clang/ASTMatchers/ASTMatchers.h>

using namespace clang::ast_matchers;

namespace nix::tidy {

void DanglingCStrCheck::registerMatchers(MatchFinder * Finder)
{
    // Match: varDecl whose initializer is a c_str()/data() call on a
    // materialized temporary of type std::string (or std::basic_string).
    //
    // The key insight: when c_str()/data() is called on a temporary,
    // the temporary is a CXXBindTemporaryExpr or MaterializeTemporaryExpr
    // that will be destroyed at the end of the full expression.
    Finder->addMatcher(
        varDecl(
            hasType(pointerType(pointee(isAnyCharacter()))),
            hasInitializer(
                cxxMemberCallExpr(
                    callee(cxxMethodDecl(hasAnyName("c_str", "data"), ofClass(hasName("::std::basic_string")))),
                    on(expr(hasType(cxxRecordDecl(hasName("::std::basic_string"))), unless(declRefExpr()))))
                    .bind("call")))
            .bind("var"),
        this);
}

void DanglingCStrCheck::check(const MatchFinder::MatchResult & Result)
{
    const auto * Var = Result.Nodes.getNodeAs<clang::VarDecl>("var");
    const auto * Call = Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("call");

    if (!Var || !Call)
        return;

    // Check that the object expression is indeed a temporary (not a
    // DeclRefExpr to a named variable). The matcher already excludes
    // direct declRefExpr, but let's also handle implicit conversions.
    const clang::Expr * Object = Call->getImplicitObjectArgument();
    if (!Object)
        return;

    Object = Object->IgnoreParenImpCasts();

    // If it's a reference to a named variable, it's fine.
    if (llvm::isa<clang::DeclRefExpr>(Object))
        return;

    // If it's a MemberExpr (e.g. this->field.c_str()), it's fine.
    if (llvm::isa<clang::MemberExpr>(Object))
        return;

    diag(
        Call->getBeginLoc(),
        "result of '%0' on a temporary std::string stored in '%1'; "
        "the pointer will dangle after the temporary is destroyed")
        << Call->getMethodDecl()->getName() << Var->getName() << Call->getSourceRange();
}

} // namespace nix::tidy
