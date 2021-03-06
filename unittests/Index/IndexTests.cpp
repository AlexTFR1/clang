//===--- IndexTests.cpp - Test indexing actions -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <memory>

namespace clang {
namespace index {
namespace {
struct Position {
  size_t Line = 0;
  size_t Column = 0;

  Position(size_t Line = 0, size_t Column = 0) : Line(Line), Column(Column) {}

  static Position fromSourceLocation(SourceLocation Loc,
                                     const SourceManager &SM) {
    FileID FID;
    unsigned Offset;
    std::tie(FID, Offset) = SM.getDecomposedSpellingLoc(Loc);
    Position P;
    P.Line = SM.getLineNumber(FID, Offset);
    P.Column = SM.getColumnNumber(FID, Offset);
    return P;
  }
};

bool operator==(const Position &LHS, const Position &RHS) {
  return std::tie(LHS.Line, LHS.Column) == std::tie(RHS.Line, RHS.Column);
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Position &Pos) {
  return OS << Pos.Line << ':' << Pos.Column;
}

struct TestSymbol {
  std::string QName;
  Position WrittenPos;
  Position DeclPos;
  // FIXME: add more information.
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const TestSymbol &S) {
  return OS << S.QName << '[' << S.WrittenPos << ']' << '@' << S.DeclPos;
}

class Indexer : public IndexDataConsumer {
public:
  void initialize(ASTContext &Ctx) override {
    AST = &Ctx;
    IndexDataConsumer::initialize(Ctx);
  }

  bool handleDeclOccurence(const Decl *D, SymbolRoleSet Roles,
                           ArrayRef<SymbolRelation>, SourceLocation Loc,
                           ASTNodeInfo) override {
    const auto *ND = llvm::dyn_cast<NamedDecl>(D);
    if (!ND)
      return true;
    TestSymbol S;
    S.QName = ND->getQualifiedNameAsString();
    S.WrittenPos = Position::fromSourceLocation(Loc, AST->getSourceManager());
    S.DeclPos =
        Position::fromSourceLocation(D->getLocation(), AST->getSourceManager());
    Symbols.push_back(std::move(S));
    return true;
  }

  bool handleMacroOccurence(const IdentifierInfo *Name, const MacroInfo *,
                            SymbolRoleSet, SourceLocation) override {
    TestSymbol S;
    S.QName = Name->getName();
    Symbols.push_back(std::move(S));
    return true;
  }

  std::vector<TestSymbol> Symbols;
  const ASTContext *AST = nullptr;
};

class IndexAction : public ASTFrontendAction {
public:
  IndexAction(std::shared_ptr<Indexer> Index,
              IndexingOptions Opts = IndexingOptions())
      : Index(std::move(Index)), Opts(Opts) {}

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    class Consumer : public ASTConsumer {
      std::shared_ptr<Indexer> Index;
      std::shared_ptr<Preprocessor> PP;
      IndexingOptions Opts;

    public:
      Consumer(std::shared_ptr<Indexer> Index, std::shared_ptr<Preprocessor> PP,
               IndexingOptions Opts)
          : Index(std::move(Index)), PP(std::move(PP)), Opts(Opts) {}

      void HandleTranslationUnit(ASTContext &Ctx) override {
        std::vector<Decl *> DeclsToIndex(
            Ctx.getTranslationUnitDecl()->decls().begin(),
            Ctx.getTranslationUnitDecl()->decls().end());
        indexTopLevelDecls(Ctx, *PP, DeclsToIndex, *Index, Opts);
      }
    };
    return llvm::make_unique<Consumer>(Index, CI.getPreprocessorPtr(), Opts);
  }

private:
  std::shared_ptr<Indexer> Index;
  IndexingOptions Opts;
};

using testing::AllOf;
using testing::Contains;
using testing::Not;
using testing::UnorderedElementsAre;

MATCHER_P(QName, Name, "") { return arg.QName == Name; }
MATCHER_P(WrittenAt, Pos, "") { return arg.WrittenPos == Pos; }
MATCHER_P(DeclAt, Pos, "") { return arg.DeclPos == Pos; }

TEST(IndexTest, Simple) {
  auto Index = std::make_shared<Indexer>();
  tooling::runToolOnCode(new IndexAction(Index), "class X {}; void f() {}");
  EXPECT_THAT(Index->Symbols, UnorderedElementsAre(QName("X"), QName("f")));
}

TEST(IndexTest, IndexPreprocessorMacros) {
  std::string Code = "#define INDEX_MAC 1";
  auto Index = std::make_shared<Indexer>();
  IndexingOptions Opts;
  Opts.IndexMacrosInPreprocessor = true;
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols, Contains(QName("INDEX_MAC")));

  Opts.IndexMacrosInPreprocessor = false;
  Index->Symbols.clear();
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols, UnorderedElementsAre());
}

TEST(IndexTest, IndexParametersInDecls) {
  std::string Code = "void foo(int bar);";
  auto Index = std::make_shared<Indexer>();
  IndexingOptions Opts;
  Opts.IndexFunctionLocals = true;
  Opts.IndexParametersInDeclarations = true;
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols, Contains(QName("bar")));

  Opts.IndexParametersInDeclarations = false;
  Index->Symbols.clear();
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols, Not(Contains(QName("bar"))));
}

TEST(IndexTest, IndexExplicitTemplateInstantiation) {
  std::string Code = R"cpp(
    template <typename T>
    struct Foo { void bar() {} };
    template <>
    struct Foo<int> { void bar() {} };
    void foo() {
      Foo<char> abc;
      Foo<int> b;
    }
  )cpp";
  auto Index = std::make_shared<Indexer>();
  IndexingOptions Opts;
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols,
              AllOf(Contains(AllOf(QName("Foo"), WrittenAt(Position(8, 7)),
                                   DeclAt(Position(5, 12)))),
                    Contains(AllOf(QName("Foo"), WrittenAt(Position(7, 7)),
                                   DeclAt(Position(3, 12))))));
}

TEST(IndexTest, IndexTemplateInstantiationPartial) {
  std::string Code = R"cpp(
    template <typename T1, typename T2>
    struct Foo { void bar() {} };
    template <typename T>
    struct Foo<T, int> { void bar() {} };
    void foo() {
      Foo<char, char> abc;
      Foo<int, int> b;
    }
  )cpp";
  auto Index = std::make_shared<Indexer>();
  IndexingOptions Opts;
  tooling::runToolOnCode(new IndexAction(Index, Opts), Code);
  EXPECT_THAT(Index->Symbols,
              Contains(AllOf(QName("Foo"), WrittenAt(Position(8, 7)),
                             DeclAt(Position(5, 12)))));
}

} // namespace
} // namespace index
} // namespace clang
