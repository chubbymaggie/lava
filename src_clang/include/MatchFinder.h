#include "FunctionArgHandler.h"
#include "MemoryAccessHandler.h"
#include "PriQueryPointHandler.h"
#include "ReadDisclosureHandler.h"
#include "FuncDuplicationHandler.h"
#include "FuncDeclArgAdditionHandler.h"
#include "FunctionPointerFieldHandler.h"
#include "CallExprArgAdditionalHandler.h"

using clang::tooling::ClangTool;
using clang::tooling::Replacement;
using clang::tooling::getAbsolutePath;
using clang::tooling::CommonOptionsParser;
using clang::tooling::SourceFileCallbacks;
using clang::tooling::TranslationUnitReplacements;


/*******************************
 * Matcher Handlers
 *******************************/

namespace clang {
    namespace ast_matchers {
        AST_MATCHER(Expr, isAttackableMatcher){
            const Expr *ce = &Node;
            return IsArgAttackable(ce);
        }

        AST_MATCHER(VarDecl, isStaticLocalDeclMatcher){
            const VarDecl *vd = &Node;
            return vd->isStaticLocal();
        }

        AST_MATCHER_P(CallExpr, forEachArgMatcher,
                internal::Matcher<Expr>, InnerMatcher) {
            BoundNodesTreeBuilder Result;
            bool Matched = false;
            for ( const auto *I : Node.arguments()) {
                //for (const auto *I : Node.inits()) {
                BoundNodesTreeBuilder InitBuilder(*Builder);
                if (InnerMatcher.matches(*I, Finder, &InitBuilder)) {
                    Matched = true;
                    Result.addMatch(InitBuilder);
                }
            }
            *Builder = std::move(Result);
            return Matched;
        }
    }
}

class LavaMatchFinder : public MatchFinder, public SourceFileCallbacks {
public:
    LavaMatchFinder() : Mod(Insert) {
        StatementMatcher memoryAccessMatcher =
            allOf(
                expr(anyOf(
                    arraySubscriptExpr(
                        hasIndex(ignoringImpCasts(
                                expr().bind("innerExpr")))),
                    unaryOperator(hasOperatorName("*"),
                        hasUnaryOperand(ignoringImpCasts(
                                expr().bind("innerExpr")))))).bind("lhs"),
                anyOf(
                    expr(hasAncestor(binaryOperator(allOf(
                                    hasOperatorName("="),
                                    hasRHS(ignoringImpCasts(
                                            expr().bind("rhs"))),
                                    hasLHS(ignoringImpCasts(expr(
                                                equalsBoundNode("lhs")))))))),
                    anything()), // this is a "maybe" construction.
                hasAncestor(functionDecl()), // makes sure that we are't in a global variable declaration
                // make sure we aren't in static local variable initializer which must be constant
                unless(hasAncestor(varDecl(isStaticLocalDeclMatcher()))));

        addMatcher(
                stmt(hasParent(compoundStmt())).bind("stmt"),
                makeHandler<PriQueryPointHandler>()
                );

        addMatcher(
                callExpr(
                    forEachArgMatcher(expr(isAttackableMatcher()).bind("arg"))),
                makeHandler<FunctionArgHandler>()
                );

        addMatcher(memoryAccessMatcher, makeHandler<MemoryAccessHandler>());

        // fortenforge's matchers (for argument addition)
        if (ArgDataflow && LavaAction == LavaInjectBugs) {
            addMatcher(
                    functionDecl().bind("funcDecl"),
                    makeHandler<FuncDeclArgAdditionHandler>());
            addMatcher(
                    callExpr().bind("callExpr"),
                    makeHandler<CallExprArgAdditionHandler>());
            addMatcher(
                    fieldDecl(anyOf(hasName("as_number"), hasName("as_string"))).bind("fieldDecl"),
                    makeHandler<FunctionPointerFieldHandler>());
        }
        addMatcher(
                callExpr(
                    callee(functionDecl(hasName("::printf"))),
                    unless(argumentCountIs(1))).bind("call_expression"),
                makeHandler<ReadDisclosureHandler>()
                );
    }

    virtual bool handleBeginSource(CompilerInstance &CI, StringRef Filename) override {
        Insert.clear();
        Mod.Reset(&CI.getLangOpts(), &CI.getSourceManager());
        TUReplace.Replacements.clear();
        TUReplace.MainSourceFile = Filename;
        CurrentCI = &CI;

        debug(INJECT) << "*** handleBeginSource for: " << Filename << "\n";

        std::string insert_at_top;
        if (LavaAction == LavaQueries) {
            insert_at_top = "#include \"pirate_mark_lava.h\"\n";
        } else if (LavaAction == LavaInjectBugs && !ArgDataflow) {
            if (main_files.count(getAbsolutePath(Filename)) > 0) {
                std::stringstream top;
                top << "static unsigned int lava_val[" << data_slots.size() << "] = {0};\n"
                    << "void lava_set(unsigned int, unsigned int);\n"
                    << "__attribute__((visibility(\"default\")))\n"
                    << "void lava_set(unsigned int slot, unsigned int val) { lava_val[slot] = val; }\n"
                    << "unsigned int lava_get(unsigned int);\n"
                    << "__attribute__((visibility(\"default\")))\n"
                    << "unsigned int lava_get(unsigned int slot) { return lava_val[slot]; }\n";
                insert_at_top = top.str();
            } else {
                insert_at_top =
                    "void lava_set(unsigned int bn, unsigned int val);\n"
                    "extern unsigned int lava_get(unsigned int);\n";
            }
        }

        debug(INJECT) << "Inserting at top of file: \n" << insert_at_top;
        TUReplace.Replacements.emplace_back(Filename, 0, 0, insert_at_top);

        for (auto it = MatchHandlers.begin();
                it != MatchHandlers.end(); it++) {
            (*it)->LangOpts = &CI.getLangOpts();
        }

        return true;
    }

    virtual void handleEndSource() override {
        debug(INJECT) << "*** handleEndSource\n";

        Insert.render(CurrentCI->getSourceManager(), TUReplace.Replacements);
        std::error_code EC;
        llvm::raw_fd_ostream YamlFile(TUReplace.MainSourceFile + ".yaml",
                EC, llvm::sys::fs::F_RW);
        yaml::Output Yaml(YamlFile);
        Yaml << TUReplace;
    }
};


class LavaMatchFinder : public MatchFinder,
                        public GenericMatchFinder,
                        public SourceFileCallbacks {
public:
    LavaMatchFinder() : GenericMatchFinder() {
        StatementMatcher memoryAccessMatcher =
            allOf(
                expr(anyOf(
                    arraySubscriptExpr(
                        hasIndex(ignoringImpCasts(
                                expr().bind("innerExpr")))),
                    unaryOperator(hasOperatorName("*"),
                        hasUnaryOperand(ignoringImpCasts(
                                expr().bind("innerExpr")))))).bind("lhs"),
                anyOf(
                    expr(hasAncestor(binaryOperator(allOf(
                                    hasOperatorName("="),
                                    hasRHS(ignoringImpCasts(
                                            expr().bind("rhs"))),
                                    hasLHS(ignoringImpCasts(expr(
                                                equalsBoundNode("lhs")))))))),
                    anything()), // this is a "maybe" construction.
                hasAncestor(functionDecl()), // makes sure that we are't in a global variable declaration
                // make sure we aren't in static local variable initializer which must be constant
                unless(hasAncestor(varDecl(isStaticLocalDeclMatcher()))));

        addMatcher(
                stmt(hasParent(compoundStmt())).bind("stmt"),
                makeHandler<PriQueryPointHandler>()
                );

        addMatcher(
                callExpr(
                    forEachArgMatcher(expr(isAttackableMatcher()).bind("arg"))),
                makeHandler<FunctionArgHandler>()
                );

        addMatcher(memoryAccessMatcher, makeHandler<MemoryAccessHandler>());

        // fortenforge's matchers (for argument addition)
        if (ArgDataflow && LavaAction == LavaInjectBugs) {
            addMatcher(
                    functionDecl().bind("funcDecl"),
                    makeHandler<FuncDeclArgAdditionHandler>());
            addMatcher(
                    callExpr().bind("callExpr"),
                    makeHandler<CallExprArgAdditionHandler>());
            addMatcher(
                    fieldDecl(anyOf(hasName("as_number"), hasName("as_string"))).bind("fieldDecl"),
                    makeHandler<FunctionPointerFieldHandler>());
        }

        addMatcher(
                callExpr(
                    callee(functionDecl(hasName("::printf"))),
                    unless(argumentCountIs(1))).bind("call_expression"),
                makeHandler<ReadDisclosureHandler>()
                );
    }

    virtual bool handleBeginSource(CompilerInstance &CI, StringRef Filename) override {
        Insert.clear();
        Mod.Reset(&CI.getLangOpts(), &CI.getSourceManager());
        TUReplace.Replacements.clear();
        TUReplace.MainSourceFile = Filename;
        CurrentCI = &CI;

        debug(INJECT) << "*** handleBeginSource for: " << Filename << "\n";

        std::stringstream logging_macros;
        logging_macros << "#ifdef LAVA_LOGGING\n" // enable logging with (LAVA_LOGGING, FULL_LAVA_LOGGING) and (DUA_LOGGING) flags. Logging requires stdio to be included
                       << "#define LAVALOG(bugid, x, trigger)  ({(trigger && fprintf(stderr, \"\\nLAVALOG: %d: %s:%d\\n\", bugid, __FILE__, __LINE__)), (x);})\n"
                       << "#endif\n"

                    << "#ifdef FULL_LAVA_LOGGING\n"
                        << "#define LAVALOG(bugid, x, trigger)  ({(trigger && fprintf(stderr, \"\\nLAVALOG: %d: %s:%d\\n\", bugid, __FILE__, __LINE__), (!trigger && fprintf(stderr, \"\\nLAVALOG_MISS: %d: %s:%d\\n\", bugid, __FILE__, __LINE__))) && fflush(0), (x);})\n"
                    << "#endif\n"

                    << "#ifndef LAVALOG\n"
                        << "#define LAVALOG(y,x,z)  (x)\n"
                    << "#endif\n"

                    << "#ifdef DUA_LOGGING\n"
                        << "#define DFLOG(idx, val)  ({fprintf(stderr, \"\\nDFLOG:%d=%d: %s:%d\\n\", idx, val, __FILE__, __LINE__) && fflush(0), data_flow[idx]=val;})\n"
                    << "#else\n"
                        << "#define DFLOG(idx, val) {data_flow[idx]=val;}\n"
                    << "#endif\n";

        std::string insert_at_top;
        if (LavaAction == LavaQueries) {
            insert_at_top = "#include \"pirate_mark_lava.h\"\n";
        } else if (LavaAction == LavaInjectBugs) {
            insert_at_top.append(logging_macros.str());
            if (!ArgDataflow) {
                if (main_files.count(getAbsolutePath(Filename)) > 0) {
                    std::stringstream top;
                    top << "static unsigned int lava_val[" << data_slots.size() << "] = {0};\n"
                        << "void lava_set(unsigned int, unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "void lava_set(unsigned int slot, unsigned int val) {\n"
                        << "#ifdef DUA_LOGGING\n"
                            << "fprintf(stderr, \"\\nlava_set:%d=%d: %s:%d\\n\", slot, val, __FILE__, __LINE__);\n"
                            << "fflush(NULL);\n"
                        << "#endif\n"
                        << "lava_val[slot] = val; }\n"
                        << "unsigned int lava_get(unsigned int);\n"
                        << "__attribute__((visibility(\"default\")))\n"
                        << "unsigned int lava_get(unsigned int slot) { return lava_val[slot]; }\n";
                    insert_at_top.append(top.str());
                } else {
                    insert_at_top.append("void lava_set(unsigned int bn, unsigned int val);\n"
                    "extern unsigned int lava_get(unsigned int);\n");
                }
            }
        }

        debug(INJECT) << "Inserting macros and lava_set/get or dataflow at top of file\n";
        TUReplace.Replacements.emplace_back(Filename, 0, 0, insert_at_top);

        for (auto it = MatchHandlers.begin();
                it != MatchHandlers.end(); it++) {
            (*it)->LangOpts = &CI.getLangOpts();
        }

        return true;
    }

    virtual void handleEndSource() override {
        debug(INJECT) << "*** handleEndSource\n";

        Insert.render(CurrentCI->getSourceManager(), TUReplace.Replacements);
        std::error_code EC;
        llvm::raw_fd_ostream YamlFile(TUReplace.MainSourceFile + ".yaml",
                EC, llvm::sys::fs::F_RW);
        yaml::Output Yaml(YamlFile);
        Yaml << TUReplace;
    }
};

