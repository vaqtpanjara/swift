//===--- TypeLowering.cpp - Type information for SILGen ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "libsil"
#include "swift/AST/ArchetypeBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Fallthrough.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace Lowering;

namespace {
  /// A CRTP type visitor for deciding whether the metatype for a type
  /// is a singleton type, i.e. whether there can only ever be one
  /// such value.
  struct HasSingletonMetatype : CanTypeVisitor<HasSingletonMetatype, bool> {
    /// Class metatypes have non-trivial representation due to the
    /// possibility of subclassing.
    bool visitClassType(CanClassType type) {
      return false;
    }
    bool visitBoundGenericClassType(CanBoundGenericClassType type) {
      return false;
    }
    bool visitDynamicSelfType(CanDynamicSelfType type) {
      return false;
    }

    /// Dependent types have non-trivial representation in case they
    /// instantiate to a class metatype.
    bool visitGenericTypeParamType(CanGenericTypeParamType type) {
      return false;
    }
    bool visitDependentMemberType(CanDependentMemberType type) {
      return false;
    }
    
    /// Archetype metatypes have non-trivial representation in case
    /// they instantiate to a class metatype.
    bool visitArchetypeType(CanArchetypeType type) {
      return false;
    }
    
    /// All levels of class metatypes support subtyping.
    bool visitMetatypeType(CanMetatypeType type) {
      return visit(type.getInstanceType());
    }

    /// Everything else is trivial.  Note that ordinary metatypes of
    /// existential types are still singleton.
    bool visitType(CanType type) {
      return true;
    }
  };
}

/// Does the metatype for the given type have a known-singleton
/// representation?
static bool hasSingletonMetatype(CanType instanceType) {
  return HasSingletonMetatype().visit(instanceType);
}

static CanType getKnownType(Optional<CanType> &cacheSlot, ASTContext &C,
                            StringRef moduleName, StringRef typeName) {
  CanType t = cacheSlot.cache([&] {
    Optional<UnqualifiedLookup> lookup
      = UnqualifiedLookup::forModuleAndName(C, moduleName, typeName);
    if (!lookup)
      return CanType();
    if (TypeDecl *typeDecl = lookup->getSingleTypeResult()) {
      assert(typeDecl->getDeclaredType() &&
             "bridged type must be type-checked");
      return typeDecl->getDeclaredType()->getCanonicalType();
    }
    return CanType();
  });

  // It is possible that we won't find a briding type (e.g. String) when we're
  // parsing the stdlib itself.
  if (t) {
    DEBUG(llvm::dbgs() << "Bridging type " << moduleName << '.' << typeName
            << " mapped to ";
          if (t)
            t->print(llvm::dbgs());
          else
            llvm::dbgs() << "<null>";
          llvm::dbgs() << '\n');
  }
  return t;
}

#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  CanType TypeConverter::get##BridgedType##Type() {         \
    return getKnownType(BridgedType##Ty, M.getASTContext(), \
                        #BridgedModule, #BridgedType);      \
  }
#include "swift/SIL/BridgedTypes.def"

AbstractionPattern TypeConverter::getMostGeneralAbstraction() {
  if (!MostGeneralArchetype) {
    MostGeneralArchetype =
      ArchetypeType::getNew(Context,
                            /*parent*/ nullptr,
                            /*associated/protocol type*/ (ProtocolDecl*) 0,
                            /*name*/ Identifier(),
                            /*conformsto*/ {},
                            /*superclass*/ Type())->getCanonicalType();
  }
  return AbstractionPattern(MostGeneralArchetype);
}

CaptureKind Lowering::getDeclCaptureKind(CaptureInfo::LocalCaptureTy capture) {
  if (VarDecl *var = dyn_cast<VarDecl>(capture.getPointer())) {
    // If captured directly, the variable is captured by box.
    if (capture.getInt()) {
      assert(var->hasStorage());
      return CaptureKind::Box;
    }

    switch (var->getStorageKind()) {
    case VarDecl::StoredWithTrivialAccessors:
      llvm_unreachable("stored local variable with trivial accessors?");
    case VarDecl::Computed:
      return var->getSetter()
        ? CaptureKind::GetterSetter : CaptureKind::Getter;
    case VarDecl::Observing:
      return CaptureKind::GetterSetter;
    case VarDecl::Stored:
      if (var->isLet())
        return CaptureKind::Constant;

      return CaptureKind::Box;
    }
  }
  
  // "Captured" local types require no context.
  if (isa<TypeAliasDecl>(capture.getPointer()))
    return CaptureKind::None;
  if (isa<GenericTypeParamDecl>(capture.getPointer()))
    return CaptureKind::None;
  if (isa<AssociatedTypeDecl>(capture.getPointer()))
    return CaptureKind::None;
  
  return CaptureKind::LocalFunction;
}

enum class LoweredTypeKind {
  /// Trivial and loadable.
  Trivial,

  /// A reference type.
  Reference,

  /// An aggregate type that contains references (potentially recursively).
  AggWithReference,

  /// Non-trivial and not loadable.
  AddressOnly
};

static LoweredTypeKind classifyType(CanType type, SILModule &M);

namespace {
  /// A CRTP helper class for doing things that depends on type
  /// classification.
  template <class Impl, class RetTy>
  class TypeClassifierBase : public CanTypeVisitor<Impl, RetTy> {
    SILModule &M;
    Impl &asImpl() { return *static_cast<Impl*>(this); }
  protected:
    TypeClassifierBase(SILModule &M) : M(M) {}

  public:
    // The subclass should implement:
    //   RetTy handleAddressOnly(CanType);
    //   RetTy handleReference(CanType);
    //   RetTy handleTrivial(CanType);
    // In addition, if it does not override visitTupleType
    // and visitAnyStructType, it should also implement:
    //   RetTy handleAggWithReference(CanType);

#define IMPL(TYPE, LOWERING)                            \
    RetTy visit##TYPE##Type(Can##TYPE##Type type) {     \
      return asImpl().handle##LOWERING(type);        \
    }

    IMPL(BuiltinInteger, Trivial)
    IMPL(BuiltinFloat, Trivial)
    IMPL(BuiltinRawPointer, Trivial)
    IMPL(BuiltinNativeObject, Reference)
    IMPL(BuiltinUnknownObject, Reference)
    IMPL(BuiltinVector, Trivial)
    IMPL(Class, Reference)
    IMPL(BoundGenericClass, Reference)
    IMPL(AnyMetatype, Trivial)
    IMPL(AnyFunction, Reference)
    IMPL(SILFunction, Reference)
    IMPL(Module, Trivial)

#undef IMPL

    RetTy visitLValueType(CanLValueType type) {
      llvm_unreachable("shouldn't get an l-value type here");
    }
    RetTy visitInOutType(CanInOutType type) {
      llvm_unreachable("shouldn't get an inout type here");
    }

    // Dependent types should be contextualized before visiting.

    RetTy visitGenericTypeParamType(CanGenericTypeParamType type) {
      llvm_unreachable("should have substituted dependent type into context");
    }
    RetTy visitDependentMemberType(CanDependentMemberType type) {
      llvm_unreachable("should have substituted dependent type into context");
    }

    RetTy visitUnmanagedStorageType(CanUnmanagedStorageType type) {
      return asImpl().handleTrivial(type);
    }

    RetTy visitUnownedStorageType(CanUnownedStorageType type) {
      return asImpl().handleReference(type);
    }

    RetTy visitWeakStorageType(CanWeakStorageType type) {
      return asImpl().handleAddressOnly(type);
    }

    // These types are address-only unless they're class-constrained.
    template <class T> RetTy visitAbstracted(T type) {
      if (type->requiresClass()) {
        return asImpl().handleReference(type);
      } else {
        return asImpl().handleAddressOnly(type);        
      }
    }
    RetTy visitArchetypeType(CanArchetypeType type) {
      return visitAbstracted(type);
    }
    RetTy visitProtocolType(CanProtocolType type) {
      return visitAbstracted(type);
    }
    RetTy visitProtocolCompositionType(CanProtocolCompositionType type) {
      return visitAbstracted(type);
    }

    // Enums depend on their enumerators.
    RetTy visitEnumType(CanEnumType type) {
      return asImpl().visitAnyEnumType(type, type->getDecl());
    }
    RetTy visitBoundGenericEnumType(CanBoundGenericEnumType type) {
      return asImpl().visitAnyEnumType(type, type->getDecl());
    }
    RetTy visitAnyEnumType(CanType type, EnumDecl *D) {
      // Consult the type lowering.
      auto &lowering = M.Types.getTypeLowering(type);
      return handleClassificationFromLowering(type, lowering);
    }
    
    RetTy handleClassificationFromLowering(CanType type,
                                           const TypeLowering &lowering) {
      if (lowering.isAddressOnly())
        return asImpl().handleAddressOnly(type);
      if (lowering.isTrivial())
        return asImpl().handleTrivial(type);
      return asImpl().handleAggWithReference(type);
    }

    // Structs depend on their physical fields.
    RetTy visitStructType(CanStructType type) {
      return asImpl().visitAnyStructType(type, type->getDecl());
    }
    RetTy visitBoundGenericStructType(CanBoundGenericStructType type) {
      return asImpl().visitAnyStructType(type, type->getDecl());
    }

    RetTy visitAnyStructType(CanType type, StructDecl *D) {
      // Consult the type lowering.  This means we implicitly get
      // caching, but that type lowering needs to override this case.
      auto &lowering = M.Types.getTypeLowering(type);
      return handleClassificationFromLowering(type, lowering);
    }
    
    // Tuples depend on their elements.
    RetTy visitTupleType(CanTupleType type) {
      bool hasReference = false;
      // TODO: We ought to be able to early-exit as soon as we've established
      // that a type is address-only. However, we also currenty rely on
      // SIL lowering to catch unsupported recursive value types.
      bool isAddressOnly = false;
      for (auto eltType : type.getElementTypes()) {
        switch (classifyType(eltType, M)) {
        case LoweredTypeKind::Trivial:
          continue;
        case LoweredTypeKind::AddressOnly:
          isAddressOnly = true;
          continue;
        case LoweredTypeKind::Reference:
        case LoweredTypeKind::AggWithReference:
          hasReference = true;
          continue;
        }
        llvm_unreachable("bad type classification");
      }

      if (isAddressOnly)
        return asImpl().handleAddressOnly(type);
      if (hasReference)
        return asImpl().handleAggWithReference(type);
      return asImpl().handleTrivial(type);
    }

    RetTy visitDynamicSelfType(CanDynamicSelfType type) {
      return this->visit(type.getSelfType());
    }
    
    RetTy visitSILBlockStorageType(CanSILBlockStorageType type) {
      // Should not be loaded.
      return asImpl().handleAddressOnly(type);
    }
  };

  class TypeClassifier :
      public TypeClassifierBase<TypeClassifier, LoweredTypeKind> {
  public:
    TypeClassifier(SILModule &M) : TypeClassifierBase(M) {}

    LoweredTypeKind handleReference(CanType type) {
      return LoweredTypeKind::Reference;
    }
    LoweredTypeKind handleAggWithReference(CanType type) {
      return LoweredTypeKind::AggWithReference;
    }
    LoweredTypeKind handleTrivial(CanType type) {
      return LoweredTypeKind::Trivial;
    }
    LoweredTypeKind handleAddressOnly(CanType type) {
      return LoweredTypeKind::AddressOnly;
    }
  };
}

static LoweredTypeKind classifyType(CanType type, SILModule &M) {
  if (type->isDependentType())
    type = M.Types.getArchetypes().substDependentType(type)->getCanonicalType();
  return TypeClassifier(M).visit(type);
}

/// True if the type, or the referenced type of an address
/// type, is address-only.  For example, it could be a resilient struct or
/// something of unknown size.
bool SILType::isAddressOnly(CanType type, SILModule &M) {
  return classifyType(type, M) == LoweredTypeKind::AddressOnly;
}

namespace {
  /// A class for loadable types.
  class LoadableTypeLowering : public TypeLowering {
  protected:
    LoadableTypeLowering(SILType type, IsTrivial_t isTrivial)
      : TypeLowering(type, isTrivial, IsNotAddressOnly) {}

  public:
    void emitDestroyAddress(SILBuilder &B, SILLocation loc,
                            SILValue addr) const override {
      SILValue value = B.createLoad(loc, addr);
      emitReleaseValue(B, loc, value);
    }

    void emitDestroyRValue(SILBuilder &B, SILLocation loc,
                           SILValue value) const override {
      emitReleaseValue(B, loc, value);
    }

    void emitCopyInto(SILBuilder &B, SILLocation loc,
                      SILValue src, SILValue dest, IsTake_t isTake,
                      IsInitialization_t isInit) const override {
      SILValue value = emitLoadOfCopy(B, loc, src, isTake);
      emitStoreOfCopy(B, loc, value, dest, isInit);
    }
  };

  /// A class for trivial, loadable types.
  class TrivialTypeLowering final : public LoadableTypeLowering {
  public:
    TrivialTypeLowering(SILType type)
      : LoadableTypeLowering(type, IsTrivial) {}

    SILValue emitLoadOfCopy(SILBuilder &B, SILLocation loc, SILValue addr,
                            IsTake_t isTake) const override {
      return B.createLoad(loc, addr);
    }

    void emitStoreOfCopy(SILBuilder &B, SILLocation loc,
                         SILValue value, SILValue addr,
                         IsInitialization_t isInit) const override {
      B.createStore(loc, value, addr);
    }

    void emitDestroyAddress(SILBuilder &B, SILLocation loc,
                            SILValue addr) const override {
      // Trivial
    }

    void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                 SILValue value,
                                 LoweringStyle loweringStyle) const override {
      // Trivial
    }

    void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                              SILValue value,
                              LoweringStyle style) const override {
      // Trivial
    }

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue value) const override {
      // Trivial
    }

    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue value) const override {
      // Trivial
    }
  };

  class NonTrivialLoadableTypeLowering : public LoadableTypeLowering {
  public:
    NonTrivialLoadableTypeLowering(SILType type)
      : LoadableTypeLowering(type, IsNotTrivial) {}

    SILValue emitLoadOfCopy(SILBuilder &B, SILLocation loc,
                            SILValue addr, IsTake_t isTake) const override {
      SILValue value = B.createLoad(loc, addr);
      if (!isTake) emitRetainValue(B, loc, value);
      return value;
    }

    void emitStoreOfCopy(SILBuilder &B, SILLocation loc,
                         SILValue newValue, SILValue addr,
                         IsInitialization_t isInit) const override {
      SILValue oldValue;
      if (!isInit) oldValue = B.createLoad(loc, addr);
      B.createStore(loc, newValue, addr);
      if (!isInit) emitReleaseValue(B, loc, oldValue);
    }
  };

  /// A CRTP helper class for loadable but non-trivial aggregate types.
  template <class Impl, class IndexType>
  class LoadableAggTypeLowering : public NonTrivialLoadableTypeLowering {
  public:
    /// A child of this aggregate type.
    class Child {
      /// The index of this child, used to project it out.
      IndexType Index;

      /// The aggregate's type lowering.
      const TypeLowering *Lowering;
    public:
      Child(IndexType index, const TypeLowering &lowering)
        : Index(index), Lowering(&lowering) {}
      const TypeLowering &getLowering() const { return *Lowering; }
      IndexType getIndex() const { return Index; }
      bool isTrivial() const { return Lowering->isTrivial(); }
    };

  private:
    const Impl &asImpl() const { return static_cast<const Impl&>(*this); }
    Impl &asImpl() { return static_cast<Impl&>(*this); }

    // A reference to the lazily-allocated children vector.
    mutable ArrayRef<Child> Children = {};
    
  protected:
    virtual void lowerChildren(SILModule &M, SmallVectorImpl<Child> &children)
      const = 0;
    
  public:
    LoadableAggTypeLowering(CanType type)
      : NonTrivialLoadableTypeLowering(SILType::getPrimitiveObjectType(type)) {
    }

    ArrayRef<Child> getChildren(SILModule &M) const {
      if (Children.data() == nullptr) {
        SmallVector<Child, 4> children;
        lowerChildren(M, children);
        auto isDependent = IsDependent_t(getLoweredType().isDependentType());
        auto buf = operator new(sizeof(Child) * children.size(), M.Types,
                                isDependent);
        memcpy(buf, children.data(), sizeof(Child) * children.size());
        Children = {reinterpret_cast<Child*>(buf), children.size()};
      }
      return Children;
    }

    template <class T>
    void forEachNonTrivialChild(SILBuilder &B, SILLocation loc,
                                SILValue aggValue,
                                const T &operation) const {
      for (auto &child : getChildren(B.getModule())) {
        auto &childLowering = child.getLowering();
        // Skip trivial children.
        if (childLowering.isTrivial())
          continue;
        auto childIndex = child.getIndex();
        auto childValue = asImpl().emitRValueProject(B, loc, aggValue,
                                                   childIndex, childLowering);
        operation(B, loc, childIndex, childValue, childLowering);
      }
    }

    using SimpleOperationTy = void (TypeLowering::*)(SILBuilder &B,
                                                     SILLocation loc,
                                                     SILValue value) const;
    void forEachNonTrivialChild(SILBuilder &B, SILLocation loc,
                                SILValue aggValue,
                                SimpleOperationTy operation) const {
      forEachNonTrivialChild(B, loc, aggValue,
        [operation](SILBuilder &B, SILLocation loc, IndexType index,
                     SILValue childValue, const TypeLowering &childLowering) {
          (childLowering.*operation)(B, loc, childValue);
        });
    }

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue aggValue) const override {
      B.createRetainValue(loc, aggValue);
    }

    void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                              SILValue aggValue,
                              LoweringStyle style) const override {
      for (auto &child : getChildren(B.getModule())) {
        auto &childLowering = child.getLowering();
        SILValue childValue = asImpl().emitRValueProject(B, loc, aggValue,
                                                         child.getIndex(),
                                                         childLowering);
        if (!childLowering.isTrivial()) {
          childLowering.emitLoweredCopyChildValue(B, loc, childValue, style);
        }
      }
    }

    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue aggValue) const override {
      B.emitReleaseValue(loc, aggValue);
    }

    void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                 SILValue aggValue,
                                 LoweringStyle loweringStyle) const override {
      SimpleOperationTy Fn;

      switch(loweringStyle) {
      case LoweringStyle::Shallow:
        Fn = &TypeLowering::emitReleaseValue;
        break;
      case LoweringStyle::Deep:
        Fn = &TypeLowering::emitLoweredReleaseValueDeep;
        break;
      case LoweringStyle::DeepNoEnum:
        Fn = &TypeLowering::emitLoweredReleaseValueDeepNoEnum;
        break;
      }

      forEachNonTrivialChild(B, loc, aggValue, Fn);
    }
  };

  /// A lowering for loadable but non-trivial tuple types.
  class LoadableTupleTypeLowering final
      : public LoadableAggTypeLowering<LoadableTupleTypeLowering, unsigned> {
  public:
    LoadableTupleTypeLowering(CanType type)
      : LoadableAggTypeLowering(type) {}

    SILValue emitRValueProject(SILBuilder &B, SILLocation loc,
                               SILValue tupleValue, unsigned index,
                               const TypeLowering &eltLowering) const {
      return B.createTupleExtract(loc, tupleValue, index,
                                  eltLowering.getLoweredType());
    }

    SILValue rebuildAggregate(SILBuilder &B, SILLocation loc,
                              ArrayRef<SILValue> values) const {
      return B.createTuple(loc, getLoweredType(), values);
    }
  
  private:
    void lowerChildren(SILModule &M, SmallVectorImpl<Child> &children)
    const override {
      // The children are just the elements of the lowered tuple.
      auto silTy = getLoweredType();
      auto tupleTy = silTy.castTo<TupleType>();
      children.reserve(tupleTy->getFields().size());
      unsigned index = 0;
      for (auto elt : tupleTy.getElementTypes()) {
        auto silElt = SILType::getPrimitiveType(elt, silTy.getCategory());
        children.push_back(Child{index, M.Types.getTypeLowering(silElt)});
        ++index;
      }
    }
  };

  /// A lowering for loadable but non-trivial struct types.
  class LoadableStructTypeLowering final
      : public LoadableAggTypeLowering<LoadableStructTypeLowering, VarDecl*> {
  public:
    LoadableStructTypeLowering(CanType type)
      : LoadableAggTypeLowering(type) {}

    SILValue emitRValueProject(SILBuilder &B, SILLocation loc,
                               SILValue structValue, VarDecl *field,
                               const TypeLowering &fieldLowering) const {
      return B.createStructExtract(loc, structValue, field,
                                   fieldLowering.getLoweredType());
    }

    SILValue rebuildAggregate(SILBuilder &B, SILLocation loc,
                              ArrayRef<SILValue> values) const {
      return B.createStruct(loc, getLoweredType(), values);
    }
        
  private:
    void lowerChildren(SILModule &M, SmallVectorImpl<Child> &children)
    const override {
      auto silTy = getLoweredType();
      auto structDecl = silTy.getStructOrBoundGenericStruct();
      assert(structDecl);
      
      for (auto prop : structDecl->getStoredProperties()) {
        // Lower the type according to the abstraction level of its original
        // declaration.
        auto origTy = AbstractionPattern(prop->getType());
        auto substTy = silTy.getSwiftRValueType()
          ->getTypeOfMember(M.getSwiftModule(), prop, nullptr);
        
        children.push_back(
                         Child{prop, M.Types.getTypeLowering(origTy, substTy)});
      }
    }
  };
  
  /// A lowering for loadable but non-trivial enum types.
  class LoadableEnumTypeLowering final : public NonTrivialLoadableTypeLowering {
  public:
    /// A non-trivial case of the enum.
    class NonTrivialElement {
      /// The non-trivial element.
      EnumElementDecl *element;
      
      /// Its type lowering.
      const TypeLowering *lowering;
      
    public:
      NonTrivialElement(EnumElementDecl *element, const TypeLowering &lowering)
        : element(element), lowering(&lowering) {}
      
      const TypeLowering &getLowering() const { return *lowering; }
      EnumElementDecl *getElement() const { return element; }
    };
    
  private:

    using SimpleOperationTy = void (*)(SILBuilder &B, SILLocation loc, SILValue value,
                                       const TypeLowering &valueLowering,
                                       SILBasicBlock *dest);

    /// Emit a value semantics operation for each nontrivial case of the enum.
    template <typename T>
    void ifNonTrivialElement(SILBuilder &B, SILLocation loc, SILValue value,
                             const T &operation) const {
      SmallVector<std::pair<EnumElementDecl*,SILBasicBlock*>, 4> nonTrivialBBs;
      
      auto &M = B.getFunction().getModule();

      // Create all the blocks up front, so we can set up our switch_enum.
      auto nonTrivialElts = getNonTrivialElements(M);
      for (auto &elt : nonTrivialElts) {
        auto bb = new (M) SILBasicBlock(&B.getFunction());
        auto argTy = elt.getLowering().getLoweredType();
        new (M) SILArgument(argTy, bb);
        nonTrivialBBs.push_back({elt.getElement(), bb});
      }

      // If we are appending to the end of a block being constructed, then we
      // create a new basic block to continue cons'ing up code.  If we're
      // emitting this operation into the middle of existing code, we split the
      // block.
      SILBasicBlock *doneBB = B.splitBlockForFallthrough();
      B.createSwitchEnum(loc, value, doneBB, nonTrivialBBs);
      
      for (size_t i = 0; i < nonTrivialBBs.size(); ++i) {
        SILBasicBlock *bb = nonTrivialBBs[i].second;
        const TypeLowering &lowering = nonTrivialElts[i].getLowering();
        B.emitBlock(bb);
        operation(B, loc, bb->getBBArgs()[0], lowering, doneBB);
      }
      
      B.emitBlock(doneBB);
    }
    
    /// A reference to the lazily-allocated array of non-trivial enum cases.
    mutable ArrayRef<NonTrivialElement> NonTrivialElements = {};
    
  public:
    LoadableEnumTypeLowering(CanType type)
      : NonTrivialLoadableTypeLowering(SILType::getPrimitiveObjectType(type))
    {
    }
    
    ArrayRef<NonTrivialElement> getNonTrivialElements(SILModule &M) const {
      SILType silTy = getLoweredType();
      EnumDecl *enumDecl = silTy.getEnumOrBoundGenericEnum();
      assert(enumDecl);
      
      if (NonTrivialElements.data() == nullptr) {
        SmallVector<NonTrivialElement, 4> elts;
        
        for (auto elt : enumDecl->getAllElements()) {
          if (!elt->hasArgumentType()) continue;
          auto origTy = AbstractionPattern(elt->getArgumentType());
          auto substTy = silTy.getSwiftRValueType()
            ->getTypeOfMember(M.getSwiftModule(), elt, nullptr,
                              elt->getArgumentInterfaceType());
          elts.push_back(NonTrivialElement{elt,
                                     M.Types.getTypeLowering(origTy, substTy)});
        }
        
        auto isDependent = IsDependent_t(silTy.isDependentType());
        
        auto buf = operator new(sizeof(NonTrivialElement) * elts.size(),
                                M.Types, isDependent);
        memcpy(buf, elts.data(), sizeof(NonTrivialElement) * elts.size());
        NonTrivialElements = {reinterpret_cast<NonTrivialElement*>(buf),
                              elts.size()};
      }
      return NonTrivialElements;
    }

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue value) const override {
      B.createRetainValue(loc, value);
    }

    void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                              SILValue value,
                              LoweringStyle style) const override {
      if (style == LoweringStyle::Shallow ||
          style == LoweringStyle::DeepNoEnum) {
        B.createRetainValue(loc, value);
      } else {
        ifNonTrivialElement(B, loc, value,
          [&](SILBuilder &B, SILLocation loc, SILValue child,
              const TypeLowering &childLowering, SILBasicBlock *dest) {
            childLowering.emitLoweredCopyChildValue(B, loc, child, style);
            B.createBranch(loc, dest);
          });
      }
    }
    
    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue value) const override {
      B.emitReleaseValue(loc, value);
    }

    void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                 SILValue value,
                                 LoweringStyle style) const override {
      assert(style != LoweringStyle::Shallow &&
             "This method should never be called when performing a shallow "
             "destroy value.");
      if (style == LoweringStyle::DeepNoEnum)
        B.emitReleaseValue(loc, value);
      else
        ifNonTrivialElement(B, loc, value,
          [&](SILBuilder &B, SILLocation loc, SILValue child,
             const TypeLowering &childLowering, SILBasicBlock *dest) {
             childLowering.emitLoweredDestroyChildValue(B, loc, child, style);
             B.createBranch(loc, dest);
          });
    }
  };

  class LeafLoadableTypeLowering : public NonTrivialLoadableTypeLowering {
  public:
    LeafLoadableTypeLowering(SILType type)
      : NonTrivialLoadableTypeLowering(type) {}

    void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                              SILValue value,
                              LoweringStyle style) const override {
      emitRetainValue(B, loc, value);
    }

    void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                 SILValue value,
                                 LoweringStyle style) const override {
      emitReleaseValue(B, loc, value);
    }
  };

  /// A class for reference types, which are all non-trivial but still
  /// loadable.
  class ReferenceTypeLowering : public LeafLoadableTypeLowering {
  public:
    ReferenceTypeLowering(SILType type) : LeafLoadableTypeLowering(type) {}

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue value) const override {
      if (!isa<FunctionRefInst>(value))
        B.createStrongRetain(loc, value);
    }

    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue value) const override {
      B.emitStrongRelease(loc, value);
    }
  };

  /// A type lowering for @unowned types.
  class UnownedTypeLowering final : public LeafLoadableTypeLowering {
  public:
    UnownedTypeLowering(SILType type) : LeafLoadableTypeLowering(type) {}

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue value) const override {
      B.createUnownedRetain(loc, value);
    }

    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue value) const override {
      B.createUnownedRelease(loc, value);
    }
  };

  /// A class for non-trivial, address-only types.
  class AddressOnlyTypeLowering : public TypeLowering {
  public:
    AddressOnlyTypeLowering(SILType type)
      : TypeLowering(type, IsNotTrivial, IsAddressOnly) {}

  public:
    void emitCopyInto(SILBuilder &B, SILLocation loc,
                      SILValue src, SILValue dest, IsTake_t isTake,
                      IsInitialization_t isInit) const override {
      B.createCopyAddr(loc, src, dest, isTake, isInit);
    }

    SILValue emitLoadOfCopy(SILBuilder &B, SILLocation loc,
                            SILValue addr, IsTake_t isTake) const override {
      llvm_unreachable("calling emitLoadOfCopy on non-loadable type");
    }

    void emitStoreOfCopy(SILBuilder &B, SILLocation loc,
                         SILValue newValue, SILValue addr,
                         IsInitialization_t isInit) const override {
      llvm_unreachable("calling emitStoreOfCopy on non-loadable type");
    }

    void emitDestroyAddress(SILBuilder &B, SILLocation loc,
                            SILValue addr) const override {
      B.emitDestroyAddr(loc, addr);
    }

    void emitDestroyRValue(SILBuilder &B, SILLocation loc,
                           SILValue value) const override {
      B.emitDestroyAddr(loc, value);
    }

    void emitRetainValue(SILBuilder &B, SILLocation loc,
                       SILValue value) const override {
      llvm_unreachable("type is not loadable!");
    }

    void emitLoweredRetainValue(SILBuilder &B, SILLocation loc,
                              SILValue value,
                              LoweringStyle style) const override {
      llvm_unreachable("type is not loadable!");
    }

    void emitReleaseValue(SILBuilder &B, SILLocation loc,
                          SILValue value) const override {
      llvm_unreachable("type is not loadable!");
    }

    void emitLoweredReleaseValue(SILBuilder &B, SILLocation loc,
                                 SILValue value,
                                 LoweringStyle style) const override {
      llvm_unreachable("type is not loadable!");
    }
  };
  
  /// A class that acts as a stand-in for unimplemented types.
  class UnimplementedTypeLowering : public AddressOnlyTypeLowering {
    using AddressOnlyTypeLowering::AddressOnlyTypeLowering;
  };

  /// Build the appropriate TypeLowering subclass for the given type.
  class LowerType
    : public TypeClassifierBase<LowerType, const TypeLowering *>
  {
    TypeConverter &TC;
    CanType OrigType;
    IsDependent_t Dependent;
  public:
    LowerType(TypeConverter &TC, CanType OrigType, IsDependent_t Dependent)
      : TypeClassifierBase(TC.M), TC(TC), OrigType(OrigType),
        Dependent(Dependent) {}
        
    const TypeLowering *handleTrivial(CanType type) {
      auto silType = SILType::getPrimitiveObjectType(OrigType);
      return new (TC, Dependent) TrivialTypeLowering(silType);
    }
  
    const TypeLowering *handleReference(CanType type) {
      auto silType = SILType::getPrimitiveObjectType(OrigType);
      return new (TC, Dependent) ReferenceTypeLowering(silType);
    }

    const TypeLowering *handleAddressOnly(CanType type) {
      auto silType = SILType::getPrimitiveAddressType(OrigType);
      return new (TC, Dependent) AddressOnlyTypeLowering(silType);
    }

    /// @unowned is basically like a reference type lowering except
    /// it manipulates unowned reference counts instead of strong.
    const TypeLowering *visitUnownedStorageType(CanUnownedStorageType type) {
      return new (TC, Dependent) UnownedTypeLowering(
                                      SILType::getPrimitiveObjectType(OrigType));
    }

    const TypeLowering *visitTupleType(CanTupleType tupleType) {
      typedef LoadableTupleTypeLowering::Child Child;
      SmallVector<Child, 8> childElts;
      // TODO: We ought to be able to early-exit as soon as we've established
      // that a type is address-only. However, we also currenty rely on
      // SIL lowering to catch unsupported recursive value types.
      bool isAddressOnly = false;
      bool hasOnlyTrivialChildren = true;

      unsigned i = 0;
      for (auto eltType : tupleType.getElementTypes()) {
        auto &lowering = TC.getTypeLowering(eltType);
        if (lowering.isAddressOnly())
          isAddressOnly = true;
        hasOnlyTrivialChildren &= lowering.isTrivial();
        childElts.push_back(Child(i, lowering));
        ++i;
      }

      if (isAddressOnly)
        return handleAddressOnly(tupleType);
      if (hasOnlyTrivialChildren)
        return handleTrivial(tupleType);
      return new (TC, Dependent) LoadableTupleTypeLowering(OrigType);
    }

    const TypeLowering *visitAnyStructType(CanType structType, StructDecl *D) {
      // TODO: We ought to be able to early-exit as soon as we've established
      // that a type is address-only. However, we also currenty rely on
      // SIL lowering to catch unsupported recursive value types.
      bool isAddressOnly = false;
      
      // For consistency, if it's anywhere resilient, we need to treat the type
      // as resilient in SIL.
      if (TC.isAnywhereResilient(D))
        isAddressOnly = true;

      // Classify the type according to its stored properties.
      bool trivial = true;
      for (auto field : D->getStoredProperties()) {
        auto substFieldType =
          structType->getTypeOfMember(D->getModuleContext(), field, nullptr);
        switch (classifyType(substFieldType->getCanonicalType(), TC.M)) {
        case LoweredTypeKind::AddressOnly:
          isAddressOnly = true;
          break;
        case LoweredTypeKind::AggWithReference:
        case LoweredTypeKind::Reference:
          trivial = false;
          break;
        case LoweredTypeKind::Trivial:
          break;
        }
      }

      if (isAddressOnly)
        return handleAddressOnly(structType);
      if (trivial)
        return handleTrivial(structType);
      return new (TC, Dependent) LoadableStructTypeLowering(OrigType);
    }
        
    const TypeLowering *visitAnyEnumType(CanType enumType, EnumDecl *D) {
      // TODO: We ought to be able to early-exit as soon as we've established
      // that a type is address-only. However, we also currenty rely on
      // SIL lowering to catch unsupported recursive value types.
      bool isAddressOnly = false;

      // For consistency, if it's anywhere resilient, we need to treat the type
      // as resilient in SIL.
      if (TC.isAnywhereResilient(D))
        isAddressOnly = true;
      
      // Lower Self? as if it were Whatever? and Self! as if it were Whatever!.
      if (auto genericEnum = dyn_cast<BoundGenericEnumType>(enumType)) {
        if (auto dynamicSelf =
              dyn_cast<DynamicSelfType>(genericEnum.getGenericArgs()[0])) {
          CanType selfType = dynamicSelf.getSelfType();
          if (D == TC.Context.getOptionalDecl()) {
            return &TC.getTypeLowering(OptionalType::get(selfType));
          } else if (D == TC.Context.getImplicitlyUnwrappedOptionalDecl()) {
            return &TC.getTypeLowering(
                                ImplicitlyUnwrappedOptionalType::get(selfType));
          }
        }
      }

      // If any of the enum elements have address-only data, the enum is
      // address-only.
      bool trivial = true;
      for (auto elt : D->getAllElements()) {
        // No-payload elements do not affect address-only-ness.
        if (!elt->hasArgumentType())
          continue;
        
        auto substEltType = enumType->getTypeOfMember(
                              D->getModuleContext(),
                              elt, nullptr,
                              elt->getArgumentInterfaceType())
          ->getCanonicalType();
        
        switch (classifyType(substEltType->getCanonicalType(), TC.M)) {
        case LoweredTypeKind::AddressOnly:
          isAddressOnly = true;
          break;
        case LoweredTypeKind::AggWithReference:
        case LoweredTypeKind::Reference:
          trivial = false;
          break;
        case LoweredTypeKind::Trivial:
          break;
        }
        
      }
      if (isAddressOnly)
        return handleAddressOnly(enumType);
      if (trivial)
        return handleTrivial(enumType);
      return new (TC, Dependent) LoadableEnumTypeLowering(OrigType);
    }

    const TypeLowering *visitDynamicSelfType(CanDynamicSelfType type) {
      return LowerType(TC, cast<DynamicSelfType>(OrigType).getSelfType(), 
                       Dependent)
               .visit(type.getSelfType());
    }

  };
}

TypeConverter::TypeConverter(SILModule &m)
  : M(m), Context(m.getASTContext()) {
}

TypeConverter::~TypeConverter() {
  assert(!GenericArchetypes.hasValue() && "generic context was never popped?!");

  // The bump pointer allocator destructor will deallocate but not destroy all
  // our independent TypeLowerings.
  for (auto &ti : IndependentTypes) {
    // Destroy only the unique entries.
    CanType srcType = CanType(ti.first.OrigType);
    CanType mappedType = ti.second->getLoweredType().getSwiftRValueType();
    if (srcType == mappedType || isa<InOutType>(srcType))
      ti.second->~TypeLowering();
  }
}

void *TypeLowering::operator new(size_t size, TypeConverter &tc,
                                 IsDependent_t dependent) {
  return dependent
    ? tc.DependentBPA.Allocate(size, alignof(TypeLowering))
    : tc.IndependentBPA.Allocate(size, alignof(TypeLowering));
}

const TypeLowering *TypeConverter::find(TypeKey k) {
  auto &Types = k.isDependent() ? DependentTypes : IndependentTypes;
  auto found = Types.find(k);
  if (found == Types.end())
    return nullptr;
  // We place a null placeholder in the hashtable to catch
  // reentrancy, which arises as a result of improper recursion.
  // TODO: We should diagnose nonterminating recursion in Sema, and implement
  // terminating recursive enums, instead of diagnosing here.
  // When that Sema check is in place, we should reinstate the early-exit
  // behavior for address-only types (marked by other TODO: items throughout
  // this file).
  if (!found->second) {
    // Try to complain about a nominal type.
    if (auto nomTy = k.SubstType.getAnyNominal()) {
      if (RecursiveNominalTypes.insert(nomTy).second)
        M.getASTContext().Diags.diagnose(nomTy->getLoc(),
                                         diag::unsupported_recursive_type,
                                         nomTy->getDeclaredTypeInContext());
    } else
      assert(false && "non-nominal types should not be recursive");
    found->second = new (*this, k.isDependent()) UnimplementedTypeLowering(
                                SILType::getPrimitiveAddressType(k.SubstType));
  }
  return found->second;
}

void TypeConverter::insert(TypeKey k, const TypeLowering *tl) {
  auto &Types = k.isDependent() ? DependentTypes : IndependentTypes;
  Types[k] = tl;
}

#ifndef NDEBUG
/// Is this type a lowered type?
static bool isLoweredType(CanType type) {
  if (isa<LValueType>(type) || isa<InOutType>(type))
    return false;
  if (isa<AnyFunctionType>(type))
    return false;
  if (auto tuple = dyn_cast<TupleType>(type)) {
    for (auto elt : tuple.getElementTypes())
      if (!isLoweredType(elt))
        return false;
    return true;
  }
  if (auto meta = dyn_cast<AnyMetatypeType>(type)) {
    return meta->hasRepresentation();
  }
  return true;
}
#endif

/// Lower each of the elements of the substituted type according to
/// the abstraction pattern of the given original type.
static CanTupleType getLoweredTupleType(TypeConverter &tc,
                                        AbstractionPattern origType,
                                        CanTupleType substType) {
  assert(origType.matchesTuple(substType));

  // Does the lowered tuple type differ from the substituted type in
  // any interesting way?
  bool changed = false;
  SmallVector<TupleTypeElt, 4> loweredElts;
  loweredElts.reserve(substType->getNumElements());

  for (auto i : indices(substType->getElementTypes())) {
    auto origEltType = origType.getTupleElementType(i);
    auto substEltType = substType.getElementType(i);

    assert(!isa<LValueType>(substEltType) &&
           "lvalue types cannot exist in function signatures");

    CanType loweredSubstEltType;
    if (auto substLV = dyn_cast<InOutType>(substEltType)) {
      SILType silType = tc.getLoweredType(origType.getLValueObjectType(),
                                          substLV.getObjectType());
      loweredSubstEltType = CanInOutType::get(silType.getSwiftRValueType());

    } else {
      // If the original type was an archetype, use that archetype as
      // the original type of the element --- the actual archetype
      // doesn't matter, just the abstraction pattern.
      SILType silType = tc.getLoweredType(origEltType, substEltType);
      loweredSubstEltType = silType.getSwiftRValueType();
    }

    changed = (changed || substEltType != loweredSubstEltType);

    auto &substElt = substType->getFields()[i];
    loweredElts.push_back(substElt.getWithType(loweredSubstEltType));
  }
  
  if (!changed) return substType;

  // Because we're transforming an existing tuple, the weird corner
  // case where TupleType::get does not return a TupleType can't happen.
  return cast<TupleType>(CanType(TupleType::get(loweredElts, tc.Context)));
}

CanSILFunctionType
TypeConverter::getSILFunctionType(AbstractionPattern origType,
                                  CanAnyFunctionType substType,
                                  unsigned uncurryLevel) {
  return getLoweredType(origType, substType, uncurryLevel)
           .castTo<SILFunctionType>();
}

const TypeLowering &
TypeConverter::getTypeLowering(AbstractionPattern origType,
                               Type origSubstType,
                               unsigned uncurryLevel) {
  CanType substType = origSubstType->getCanonicalType();
  auto key = getTypeKey(origType, substType, uncurryLevel);
  
  assert(!key.isDependent() || GenericArchetypes.hasValue()
         && "dependent type outside of generic context?!");
  
  if (auto existing = find(key))
    return *existing;
  
  // Static metatypes are unitary and can optimized to a "thin" empty
  // representation if the type also appears as a static metatype in the
  // original abstraction pattern.
  if (auto substMeta = dyn_cast<MetatypeType>(substType)) {
    SILType loweredTy;
    
    // If the metatype has already been lowered, it will already carry its
    // representation.
    if (substMeta->hasRepresentation()) {
      loweredTy = SILType::getPrimitiveObjectType(substMeta);
    } else {
      MetatypeRepresentation repr;
      
      auto origMeta = dyn_cast<MetatypeType>(origType.getAsType());
      if (!origMeta) {
        // If the metatype matches a dependent type, it must be thick.
        assert((isa<SubstitutableType>(origType.getAsType())
                || isa<DependentMemberType>(origType.getAsType()))
           && "metatype matches in position that isn't a dependent type "
              "or metatype?!");
        repr = MetatypeRepresentation::Thick;
      } else {
        // Otherwise, we're thin if the metatype is thinnable both
        // substituted and in the abstraction pattern.
        if (hasSingletonMetatype(substMeta.getInstanceType())
            && hasSingletonMetatype(origMeta.getInstanceType()))
          repr = MetatypeRepresentation::Thin;
        else
          repr = MetatypeRepresentation::Thick;
      }
      
      CanType instanceType = substMeta.getInstanceType();
      // If this is a DynamicSelf metatype, turn it into a metatype of the
      // underlying self type.
      if (auto dynamicSelf = dyn_cast<DynamicSelfType>(instanceType)) {
        instanceType = dynamicSelf.getSelfType();
      }
      
      // Regardless of thinness, metatypes are always trivial.
      auto thinnedTy = CanMetatypeType::get(instanceType, repr);
      loweredTy = SILType::getPrimitiveObjectType(thinnedTy);
    }
    
    auto *theInfo
      = new (*this, key.isDependent()) TrivialTypeLowering(loweredTy);
    insert(key, theInfo);
    return *theInfo;
  }

  // Give existential metatypes @thick representation by default.
  if (auto existMetatype = dyn_cast<ExistentialMetatypeType>(substType)) {
    CanType loweredType;
    if (existMetatype->hasRepresentation()) {
      loweredType = existMetatype;
    } else {
      loweredType =
        CanExistentialMetatypeType::get(existMetatype.getInstanceType(),
                                        MetatypeRepresentation::Thick);
    }

    auto loweredTy = SILType::getPrimitiveObjectType(loweredType);
    auto theInfo
      = new (*this, key.isDependent()) TrivialTypeLowering(loweredTy);
    insert(key, theInfo);
    return *theInfo;
  }

  // AST function types are turned into SIL function types:
  //   - the type is uncurried as desired
  //   - types are turned into their unbridged equivalents, depending
  //     on the abstract CC
  //   - ownership conventions are deduced
  if (auto substFnType = dyn_cast<AnyFunctionType>(substType)) {
    // First, lower at the AST level by uncurrying and substituting
    // bridged types.
    auto substLoweredType =
      getLoweredASTFunctionType(substFnType, uncurryLevel);

    bool canReabstract;
    switch (substLoweredType->getAbstractCC()) {
    case AbstractCC::C:
    case AbstractCC::ObjCMethod:
      // C functions cannot be reabstracted; if we were to call them
      // generically, we'd have to dynamically form an invocation in their
      // original calling convention.
      canReabstract = false;
      break;
        
    case AbstractCC::Freestanding:
    case AbstractCC::Method:
    case AbstractCC::WitnessMethod:
      // Native functions can be reabstracted freely.
      canReabstract = true;
      break;
    }

    AbstractionPattern origLoweredType = [&] {
      if (!canReabstract || substType == origType.getAsType()) {
        return AbstractionPattern(substLoweredType);
      } else if (origType.isOpaque()) {
        return origType;
      } else {
        auto origFnType = cast<AnyFunctionType>(origType.getAsType());
        return AbstractionPattern(getLoweredASTFunctionType(origFnType,
                                                            uncurryLevel));
      }
    }();
    TypeKey loweredKey = getTypeKey(origLoweredType, substLoweredType, 0);

    // If the uncurrying/unbridging process changed the type, re-check
    // the cache and add a cache entry for the curried type.
    if (substLoweredType != substType) {
      auto &typeInfo = getTypeLoweringForLoweredFunctionType(loweredKey);
      insert(key, &typeInfo);
      return typeInfo;
    }

    // If it didn't, use the standard logic.
    return getTypeLoweringForUncachedLoweredFunctionType(loweredKey);
  }

  assert(uncurryLevel == 0);

  // inout types are a special case for lowering, because they get
  // completely removed and represented as 'address' SILTypes.
  if (auto substInOutType = dyn_cast<InOutType>(substType)) {
    // Derive SILType for InOutType from the object type.
    CanType substObjectType = substInOutType.getObjectType();
    AbstractionPattern origObjectType = origType.getLValueObjectType();

    SILType loweredType = getLoweredType(origObjectType, substObjectType,
                                         uncurryLevel).getAddressType();

    auto *theInfo = new (*this, key.isDependent())
      TrivialTypeLowering(loweredType);
    insert(key, theInfo);
    return *theInfo;
  }

  // We need to lower function and metatype types within tuples.
  if (auto substTupleType = dyn_cast<TupleType>(substType)) {
    auto loweredType = getLoweredTupleType(*this, origType, substTupleType);

    // If that changed anything, check for a lowering at the lowered
    // type.
    if (loweredType != substType) {
      TypeKey loweredKey = getTypeKey(origType, loweredType, 0);
      auto &lowering = getTypeLoweringForLoweredType(loweredKey);
      insert(key, &lowering);
      return lowering;
    }

    // Okay, the lowered type didn't change anything from the subst type.
    // Fall out into the normal path.
  }

  // The Swift type directly corresponds to the lowered type; don't
  // re-check the cache.
  return getTypeLoweringForUncachedLoweredType(key);
}

const TypeLowering &TypeConverter::getTypeLowering(SILType type) {
  auto loweredType = type.getSwiftRValueType();
  auto key = getTypeKey(AbstractionPattern(loweredType), loweredType, 0);

  return getTypeLoweringForLoweredType(key);
}

const TypeLowering &
TypeConverter::getTypeLoweringForLoweredType(TypeKey key) {
  auto type = key.SubstType;
  assert(isLoweredType(type) && "type is not lowered!");
  (void)type;

  // Re-using uncurry level 0 is reasonable because our uncurrying
  // transforms are idempotent at this level.  This means we don't
  // need a ton of redundant entries in the map.
  if (auto existing = find(key))
    return *existing;

  return getTypeLoweringForUncachedLoweredType(key);
}

const TypeLowering &
TypeConverter::getTypeLoweringForLoweredFunctionType(TypeKey key) {
  assert(isa<AnyFunctionType>(key.SubstType));
  assert(key.UncurryLevel == 0);

  // Check for an existing mapping for the key.
  if (auto existing = find(key))
    return *existing;

  // Okay, we didn't find one; go ahead and produce a SILFunctionType.
  return getTypeLoweringForUncachedLoweredFunctionType(key);
}

const TypeLowering &TypeConverter::
getTypeLoweringForUncachedLoweredFunctionType(TypeKey key) {
  assert(isa<AnyFunctionType>(key.SubstType));
  assert(key.UncurryLevel == 0);

  // Catch recursions.
  // FIXME: These should be bugs, so we shouldn't need to do this in release
  // builds.
  insert(key, nullptr);
  
  // Generic functions aren't first-class values and shouldn't end up lowered
  // through this interface.
  assert(!isa<PolymorphicFunctionType>(key.SubstType)
         && !isa<GenericFunctionType>(key.SubstType));

  // Construct the SILFunctionType.
  CanType silFnType = getNativeSILFunctionType(M, key.OrigType,
                                       cast<AnyFunctionType>(key.SubstType),
                                       cast<AnyFunctionType>(key.SubstType));

  // Do a cached lookup under yet another key, just so later lookups
  // using the SILType will find the same TypeLowering object.
  auto loweredKey = getTypeKey(AbstractionPattern(key.OrigType), silFnType, 0);
  auto &lowering = getTypeLoweringForLoweredType(loweredKey);
  insert(key, &lowering);
  return lowering;
}

/// Do type-lowering for a lowered type which is not already in the cache.
const TypeLowering &
TypeConverter::getTypeLoweringForUncachedLoweredType(TypeKey key) {
  assert(!find(key) && "re-entrant or already cached");
  assert(isLoweredType(key.SubstType) && "didn't lower out l-value type?");

  // Catch reentrancy bugs.
  // FIXME: These should be bugs, so we shouldn't need to do this in release
  // builds.
  insert(key, nullptr);

  CanType contextType = key.SubstType;
  if (contextType->isDependentType())
    contextType = getArchetypes().substDependentType(contextType)
      ->getCanonicalType();
  auto *theInfo = LowerType(*this, key.SubstType,
                            key.isDependent()).visit(contextType);
  insert(key, theInfo);
  return *theInfo;
}

namespace {
  using PrimaryArchetypeMap
    = llvm::DenseMap<ArchetypeType *, std::pair<unsigned, unsigned>>;
}

static CanType
mapArchetypeToInterfaceType(const PrimaryArchetypeMap &primaryArchetypes,
                            ArchetypeType *arch) {
  auto &C = arch->getASTContext();

  // If the archetype is primary, try to map it to a generic type parameter.
  if (arch->isPrimary()) {
    auto foundArchetype = primaryArchetypes.find(arch);
    if (foundArchetype == primaryArchetypes.end()) return CanType(arch);

    return CanGenericTypeParamType::get(foundArchetype->second.first,
                                        foundArchetype->second.second, C);
  }
  
  // Otherwise, map it to a dependent member type of the parent archetype.
  assert(arch->getAssocType());
  auto base = mapArchetypeToInterfaceType(primaryArchetypes, arch->getParent());
  if (base->isDependentType())
    return CanDependentMemberType::get(base, arch->getAssocType(), C);
  assert(base == CanType(arch->getParent())
         && "substituted to non-dependent type?!");
  return CanType(arch);
}

/// Map a contextual type out of its context into a dependent generic type.
CanType
TypeConverter::getInterfaceTypeOutOfContext(CanType contextTy,
                                         DeclContext *context) const {
  return getInterfaceTypeOutOfContext(contextTy,
                                   context->getGenericParamsOfContext());
}

CanType
TypeConverter::getInterfaceTypeOutOfContext(CanType contextTy,
                                        GenericParamList *contextParams) const {
  // If the context is non-generic, we're done.
  if (!contextParams)
    return contextTy;

  // Collect the depths and indices of the primary archetypes of our generic
  // context.
  llvm::DenseMap<ArchetypeType *, std::pair<unsigned, unsigned>>
    primaryArchetypes;
  unsigned depth = contextParams->getDepth();
  do {
    for (unsigned index : indices(contextParams->getParams())) {
      ArchetypeType *archetype  = contextParams->getPrimaryArchetypes()[index];
      primaryArchetypes[archetype] = {depth, index};
    }
    contextParams = contextParams->getOuterParameters();
  } while (depth-- > 0);

  // Substitute archetypes from the context with dependent types.
  return contextTy.transform([&](Type t) -> Type {
    CanType ct(t);
    ArchetypeType *arch = dyn_cast<ArchetypeType>(ct);
    if (!arch) return t;
    return mapArchetypeToInterfaceType(primaryArchetypes, arch);
  })->getCanonicalType();
}

/// Get the type of a global variable accessor function, () -> RawPointer.
static CanAnyFunctionType getGlobalAccessorType(CanType varType) {
  ASTContext &C = varType->getASTContext();
  return CanFunctionType::get(TupleType::getEmpty(C), C.TheRawPointerType);
}

/// Get the type of a default argument generator, () -> T.
static CanAnyFunctionType getDefaultArgGeneratorType(TypeConverter &TC,
                                                     AbstractFunctionDecl *AFD,
                                                     unsigned DefaultArgIndex,
                                                     ASTContext &context) {
  auto resultTy = AFD->getDefaultArg(DefaultArgIndex).second->getCanonicalType();
  assert(resultTy && "Didn't find default argument?");
  
  // Get the generic parameters from the surrounding context.
  auto genericParams
    = TC.getConstantInfo(SILDeclRef(AFD))
        .ContextGenericParams;
  
  if (genericParams)
    return CanPolymorphicFunctionType::get(TupleType::getEmpty(context),
                                           resultTy,
                                           genericParams,
                                           AnyFunctionType::ExtInfo());

  return CanFunctionType::get(TupleType::getEmpty(context), resultTy);
}

/// Get the type of a default argument generator, () -> T.
static CanAnyFunctionType getDefaultArgGeneratorInterfaceType(
                                                     TypeConverter &TC,
                                                     AbstractFunctionDecl *AFD,
                                                     unsigned DefaultArgIndex,
                                                     ASTContext &context) {
  auto resultTy = AFD->getDefaultArg(DefaultArgIndex).second->getCanonicalType();
  assert(resultTy && "Didn't find default argument?");
  
  // Get the generic signature from the surrounding context.
  auto funcInfo = TC.getConstantInfo(SILDeclRef(AFD));
  CanGenericSignature sig;
  if (auto genTy = funcInfo.FormalInterfaceType->getAs<GenericFunctionType>()) {
    sig = genTy->getGenericSignature()->getCanonicalSignature();
    resultTy = TC.getInterfaceTypeOutOfContext(resultTy,
                                            funcInfo.ContextGenericParams);
  }
  
  if (sig)
    return CanGenericFunctionType::get(sig,
                                       TupleType::getEmpty(context),
                                       resultTy,
                                       AnyFunctionType::ExtInfo());
  
  return CanFunctionType::get(TupleType::getEmpty(context), resultTy);
}

/// Get the type of a destructor function.
static CanAnyFunctionType getDestructorType(DestructorDecl *dd,
                                            bool isDeallocating,
                                            ASTContext &C,
                                            bool isForeign) {
  auto classType = dd->getDeclContext()->getDeclaredTypeInContext()
                     ->getCanonicalType();
  
  AnyFunctionType::ExtInfo extInfo;
  if (isForeign) {
    assert(isDeallocating && "There are no foreign destroying destructors");
    extInfo = AnyFunctionType::ExtInfo(AbstractCC::ObjCMethod,
                                       FunctionType::Representation::Thin,
                                       /*noreturn*/ false);
  } else {
    extInfo = AnyFunctionType::ExtInfo(AbstractCC::Method,
                                       FunctionType::Representation::Thin,
                                       /*noreturn*/ false);
  }
  
  CanType resultTy = isDeallocating? TupleType::getEmpty(C)->getCanonicalType()
                                   : CanType(C.TheNativeObjectType);

  if (auto params = dd->getDeclContext()->getGenericParamsOfContext())
    return CanPolymorphicFunctionType::get(classType, resultTy, params, extInfo);

  return CanFunctionType::get(classType, resultTy, extInfo);
}

/// Get the type of a destructor function.
static CanAnyFunctionType getDestructorInterfaceType(DestructorDecl *dd,
                                            bool isDeallocating,
                                            ASTContext &C,
                                            bool isForeign) {
  auto classType = dd->getDeclContext()->getDeclaredInterfaceType()
                     ->getCanonicalType();

  AnyFunctionType::ExtInfo extInfo;
  if (isForeign) {
    assert(isDeallocating && "There are no foreign destroying destructors");
    extInfo = AnyFunctionType::ExtInfo(AbstractCC::ObjCMethod,
                                       FunctionType::Representation::Thin,
                                       /*noreturn*/ false);
  } else {
    extInfo = AnyFunctionType::ExtInfo(AbstractCC::Method,
                                       FunctionType::Representation::Thin,
                                       /*noreturn*/ false);
  }
  
  CanType resultTy = isDeallocating? TupleType::getEmpty(C)->getCanonicalType()
                                   : CanType(C.TheNativeObjectType);

  auto sig = dd->getDeclContext()->getGenericSignatureOfContext();
  if (sig)
    return cast<GenericFunctionType>(
      GenericFunctionType::get(sig, classType, resultTy, extInfo)
      ->getCanonicalType());
  return CanFunctionType::get(classType, resultTy, extInfo);
}

/// Retrieve the type of the ivar initializer or destroyer method for
/// a class.
static CanAnyFunctionType getIVarInitDestroyerType(ClassDecl *cd, 
                                                   bool isObjC,
                                                   ASTContext &ctx,
                                                   bool isDestroyer) {
  auto classType = cd->getDeclaredTypeInContext()->getCanonicalType();

  auto emptyTupleTy = TupleType::getEmpty(ctx)->getCanonicalType();
  CanType resultType = isDestroyer? emptyTupleTy : classType;
  auto extInfo = AnyFunctionType::ExtInfo(isObjC? AbstractCC::ObjCMethod
                                                : AbstractCC::Method,
                                          FunctionType::Representation::Thin,
                                          /*noreturn*/ false);
  resultType = CanFunctionType::get(emptyTupleTy, resultType, extInfo);
  if (auto params = cd->getGenericParams())
    return CanPolymorphicFunctionType::get(classType, resultType, params, 
                                           extInfo);
  return CanFunctionType::get(classType, resultType, extInfo);
}

/// Retrieve the type of the ivar initializer or destroyer method for
/// a class.
static CanAnyFunctionType getIVarInitDestroyerInterfaceType(ClassDecl *cd,
                                                   bool isObjC,
                                                   ASTContext &ctx,
                                                   bool isDestroyer) {
  auto classType = cd->getDeclaredInterfaceType()->getCanonicalType();

  auto emptyTupleTy = TupleType::getEmpty(ctx)->getCanonicalType();
  CanType resultType = isDestroyer? emptyTupleTy : classType;
  auto extInfo = AnyFunctionType::ExtInfo(isObjC? AbstractCC::ObjCMethod
                                                : AbstractCC::Method,
                                          FunctionType::Representation::Thin,
                                          /*noreturn*/ false);
  resultType = CanFunctionType::get(emptyTupleTy, resultType, extInfo);
  auto sig = cd->getGenericSignatureOfContext();
  if (sig)
    return CanGenericFunctionType::get(sig->getCanonicalSignature(),
                                       classType, resultType,
                                       extInfo);
  return CanFunctionType::get(classType, resultType, extInfo);
}

GenericParamList *
TypeConverter::getEffectiveGenericParamsForContext(DeclContext *dc) {

  // FIXME: This is a clunky way of uncurrying nested type parameters from
  // a function context.
  if (auto func = dyn_cast<AbstractFunctionDecl>(dc)) {
    return getConstantInfo(SILDeclRef(func)).ContextGenericParams;
  }

  if (auto closure = dyn_cast<AbstractClosureExpr>(dc)) {
    return getConstantInfo(SILDeclRef(closure)).ContextGenericParams;
  }

  return dc->getGenericParamsOfContext();
}

CanGenericSignature
TypeConverter::getEffectiveGenericSignatureForContext(DeclContext *dc) {
  // Find the innermost context that has a generic parameter list.
  // FIXME: This is wrong for generic local functions in generic contexts.
  // Their GenericParamList is not semantically a child of the context
  // GenericParamList because they "capture" their context's already-bound
  // archetypes.
  while (!dc->getGenericSignatureOfContext()) {
    dc = dc->getParent();
    if (!dc) return nullptr;
  }
  
  auto sig = dc->getGenericSignatureOfContext();
  if (!sig)
    return nullptr;
  return sig->getCanonicalSignature();
}

CanAnyFunctionType
TypeConverter::getFunctionTypeWithCaptures(CanAnyFunctionType funcType,
                                 ArrayRef<CaptureInfo::LocalCaptureTy> captures,
                                           DeclContext *parentContext) {
  // Capture generic parameters from the enclosing context.
  GenericParamList *genericParams
    = getEffectiveGenericParamsForContext(parentContext);

  if (captures.empty()) {
    if (!genericParams)
      return adjustFunctionType(funcType,
                                FunctionType::Representation::Thin);

    auto extInfo = AnyFunctionType::ExtInfo(AbstractCC::Freestanding,
                                            FunctionType::Representation::Thin,
                                            /*noreturn*/ false);

    return CanPolymorphicFunctionType::get(funcType.getInput(),
                                           funcType.getResult(),
                                           genericParams, extInfo);

  }

  SmallVector<TupleTypeElt, 8> inputFields;

  for (auto capture : captures) {
    auto VD = capture.getPointer();
    // A capture of a 'var' or 'inout' variable is done with the underlying
    // object type.
    auto captureType =
      VD->getType()->getLValueOrInOutObjectType()->getCanonicalType();

    switch (getDeclCaptureKind(capture)) {
    case CaptureKind::None:
      break;
        
    case CaptureKind::LocalFunction:
      // Local functions are captured by value.
      inputFields.push_back(TupleTypeElt(captureType));
      break;
    case CaptureKind::GetterSetter: {
      // Capture the setter and getter closures.
      Type setterTy = cast<AbstractStorageDecl>(VD)->getSetter()->getType();
      inputFields.push_back(TupleTypeElt(setterTy));
      SWIFT_FALLTHROUGH;
    }
    case CaptureKind::Getter: {
      // Capture the getter closure.
      Type getterTy = cast<AbstractStorageDecl>(VD)->getGetter()->getType();
      inputFields.push_back(TupleTypeElt(getterTy));
      break;
    }
    case CaptureKind::Constant:
      if (!getTypeLowering(captureType).isAddressOnly()) {
        // Capture the value directly, unless it is address only.
        inputFields.push_back(TupleTypeElt(captureType));
        break;
      }
      SWIFT_FALLTHROUGH;
    case CaptureKind::Box: {
      // Capture the owning NativeObject and the address of the value.
      inputFields.push_back(Context.TheNativeObjectType);
      auto lvType = CanInOutType::get(captureType);
      inputFields.push_back(TupleTypeElt(lvType));
      break;
    }
    }
  }
  
  CanType capturedInputs =
    TupleType::get(inputFields, Context)->getCanonicalType();

  auto extInfo = AnyFunctionType::ExtInfo(AbstractCC::Freestanding,
                                          FunctionType::Representation::Thin,
                                          /*noreturn*/ false);

  if (genericParams)
    return CanPolymorphicFunctionType::get(capturedInputs, funcType,
                                           genericParams, extInfo);
  
  return CanFunctionType::get(capturedInputs, funcType, extInfo);
}

CanAnyFunctionType
TypeConverter::getFunctionInterfaceTypeWithCaptures(CanAnyFunctionType funcType,
                                 ArrayRef<CaptureInfo::LocalCaptureTy> captures,
                                           DeclContext *context) {
  // Capture generic parameters from the enclosing context.
  CanGenericSignature genericSig
    = getEffectiveGenericSignatureForContext(context);
  
  if (captures.empty()) {
    if (!genericSig)
      return adjustFunctionType(funcType,
                                FunctionType::Representation::Thin);
    
    auto extInfo = AnyFunctionType::ExtInfo(AbstractCC::Freestanding,
                                            FunctionType::Representation::Thin,
                                            /*noreturn*/ false);

    return CanGenericFunctionType::get(genericSig,
                                       funcType.getInput(),
                                       funcType.getResult(),
                                       extInfo);
  }

  SmallVector<TupleTypeElt, 8> inputFields;

  for (auto capture : captures) {
    // A capture of a 'var' or 'inout' variable is done with the underlying
    // object type.
    auto vd = capture.getPointer();
    auto captureType =
      vd->getType()->getLValueOrInOutObjectType()->getCanonicalType();

    switch (getDeclCaptureKind(capture)) {
    case CaptureKind::None:
      break;
        
    case CaptureKind::LocalFunction:
      // Local functions are captured by value.
      inputFields.push_back(TupleTypeElt(captureType));
      break;
    case CaptureKind::GetterSetter: {
      // Capture the setter and getter closures.
      Type setterTy = cast<AbstractStorageDecl>(vd)->getSetter()->getInterfaceType();
      inputFields.push_back(TupleTypeElt(setterTy));
      SWIFT_FALLTHROUGH;
    }
    case CaptureKind::Getter: {
      // Capture the getter closure.
      Type getterTy =
        cast<AbstractStorageDecl>(vd)->getGetter()->getInterfaceType();

      inputFields.push_back(TupleTypeElt(getterTy));
      break;
    }
    case CaptureKind::Constant:
      if (!getTypeLowering(captureType).isAddressOnly()) {
        // Capture the value directly, unless it is address only.
        inputFields.push_back(TupleTypeElt(captureType));
        break;
      }
      SWIFT_FALLTHROUGH;
    case CaptureKind::Box: {
      // Capture the owning NativeObject and the address of the value.
      inputFields.push_back(Context.TheNativeObjectType);
      auto lvType = CanInOutType::get(captureType);
      inputFields.push_back(TupleTypeElt(lvType));
      break;
    }
    }
  }
  
  CanType capturedInputs =
    TupleType::get(inputFields, Context)->getCanonicalType();
  
  // Map context archetypes out of the captures.
  capturedInputs = getInterfaceTypeOutOfContext(capturedInputs, context);

  auto extInfo = AnyFunctionType::ExtInfo(AbstractCC::Freestanding,
                                          FunctionType::Representation::Thin,
                                          /*noreturn*/ false);

  if (genericSig)
    return CanGenericFunctionType::get(genericSig,
                                       capturedInputs, funcType,
                                       extInfo);
  
  return CanFunctionType::get(capturedInputs, funcType, extInfo);
}

CanAnyFunctionType
TypeConverter::getConstantFormalTypeWithoutCaptures(SILDeclRef c) {
  return makeConstantType(c, /*withCaptures*/ false);
}

/// Replace any DynamicSelf types with their underlying Self type.
static Type replaceDynamicSelfWithSelf(Type t) {
  return t.transform([](Type type) -> Type {
    if (auto dynamicSelf = type->getAs<DynamicSelfType>())
      return dynamicSelf->getSelfType();
    return type;
  });
}

/// Replace any DynamicSelf types with their underlying Self type.
static CanType replaceDynamicSelfWithSelf(CanType t) {
  return replaceDynamicSelfWithSelf(Type(t))->getCanonicalType();
}

CanAnyFunctionType TypeConverter::makeConstantInterfaceType(SILDeclRef c,
                                                            bool withCaptures) {
  ValueDecl *vd = c.loc.dyn_cast<ValueDecl *>();

  switch (c.kind) {
  case SILDeclRef::Kind::Func: {
    SmallVector<CaptureInfo::LocalCaptureTy, 4> captures;
    if (auto *ACE = c.loc.dyn_cast<AbstractClosureExpr *>()) {
      // TODO: Substitute out archetypes from the enclosing context with generic
      // parameters.
      auto funcTy = cast<AnyFunctionType>(ACE->getType()->getCanonicalType());
      funcTy = cast<AnyFunctionType>(
                           getInterfaceTypeOutOfContext(funcTy, ACE->getParent()));
      if (!withCaptures) return funcTy;
      ACE->getCaptureInfo().getLocalCaptures(nullptr, captures);
      return getFunctionInterfaceTypeWithCaptures(funcTy, captures,
                                                  ACE->getParent());
    }

    FuncDecl *func = cast<FuncDecl>(vd);
    auto funcTy = cast<AnyFunctionType>(
                                  func->getInterfaceType()->getCanonicalType());
    if (func->getParent() && func->getParent()->isLocalContext())
      funcTy = cast<AnyFunctionType>(
                         getInterfaceTypeOutOfContext(funcTy, func->getParent()));
    funcTy = cast<AnyFunctionType>(replaceDynamicSelfWithSelf(funcTy));
    if (!withCaptures) return funcTy;
    func->getLocalCaptures(captures);
    return getFunctionInterfaceTypeWithCaptures(funcTy, captures,
                                                func);
  }

  case SILDeclRef::Kind::Allocator:
  case SILDeclRef::Kind::EnumElement:
    return cast<AnyFunctionType>(vd->getInterfaceType()->getCanonicalType());
  
  case SILDeclRef::Kind::Initializer:
    return cast<AnyFunctionType>(cast<ConstructorDecl>(vd)
                           ->getInitializerInterfaceType()->getCanonicalType());
  
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator:
    return getDestructorInterfaceType(cast<DestructorDecl>(vd),
                             c.kind == SILDeclRef::Kind::Deallocator,
                             Context,
                             c.isForeign);
  
  case SILDeclRef::Kind::GlobalAccessor: {
    VarDecl *var = cast<VarDecl>(vd);
    assert(var->hasStorage() &&
           "constant ref to computed global var");
    return getGlobalAccessorType(var->getInterfaceType()->getCanonicalType());
  }
  case SILDeclRef::Kind::DefaultArgGenerator:
    return getDefaultArgGeneratorInterfaceType(*this,
                                      cast<AbstractFunctionDecl>(vd),
                                      c.defaultArgIndex, Context);
  case SILDeclRef::Kind::IVarInitializer:
    return getIVarInitDestroyerInterfaceType(cast<ClassDecl>(vd),
                                             c.isForeign, Context, false);
  case SILDeclRef::Kind::IVarDestroyer:
    return getIVarInitDestroyerInterfaceType(cast<ClassDecl>(vd),
                                             c.isForeign, Context, true);
  }
}

/// Get the context generic parameters for an entity.
std::pair<GenericParamList *, GenericParamList *>
TypeConverter::getConstantContextGenericParams(SILDeclRef c,
                                               bool withCaptures) {
  ValueDecl *vd = c.loc.dyn_cast<ValueDecl *>();
  
  /// Get the function generic params, including outer params if withCaptures is
  /// set.
  auto getLocalFuncParams = [&](FuncDecl *func) -> GenericParamList * {
    // FIXME: For local generic functions we need to chain the local generic
    // context to the outer context.
    if (auto GP = func->getGenericParamsOfContext())
      return GP;
    return getEffectiveGenericParamsForContext(func->getParent());
  };
  
  switch (c.kind) {
  case SILDeclRef::Kind::Func: {
    if (auto *ACE = c.getAbstractClosureExpr()) {
      // Closures are currently never natively generic.
      if (!withCaptures) return {nullptr, nullptr};
      return {getEffectiveGenericParamsForContext(ACE->getParent()),
              nullptr};
    }
    FuncDecl *func = cast<FuncDecl>(vd);
    return {getLocalFuncParams(func), func->getGenericParams()};
  }
  case SILDeclRef::Kind::EnumElement: {
    auto eltDecl = cast<EnumElementDecl>(vd);
    return {
      eltDecl->getDeclContext()->getGenericParamsOfContext(),
      nullptr
    };
  }
  case SILDeclRef::Kind::Allocator:
  case SILDeclRef::Kind::Initializer:
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator: {
    auto *afd = cast<AbstractFunctionDecl>(vd);
    return {afd->getGenericParamsOfContext(), afd->getGenericParams()};
  }
  case SILDeclRef::Kind::GlobalAccessor: {
    return {
      cast<VarDecl>(vd)->getDeclContext()->getGenericParamsOfContext(),
      nullptr,
    };
  }
  case SILDeclRef::Kind::IVarInitializer:
  case SILDeclRef::Kind::IVarDestroyer:
    return {cast<ClassDecl>(vd)->getGenericParamsOfContext(), nullptr};
  case SILDeclRef::Kind::DefaultArgGenerator:
    // Use the context generic parameters of the original declaration.
    return getConstantContextGenericParams(SILDeclRef(c.getDecl()),
                                           /*captures*/ false);
  }
}

CanAnyFunctionType TypeConverter::makeConstantType(SILDeclRef c,
                                                   bool withCaptures) {
  ValueDecl *vd = c.loc.dyn_cast<ValueDecl *>();

  switch (c.kind) {
  case SILDeclRef::Kind::Func: {
    SmallVector<CaptureInfo::LocalCaptureTy, 4> captures;
    if (auto *ACE = c.loc.dyn_cast<AbstractClosureExpr *>()) {
      auto funcTy = cast<AnyFunctionType>(ACE->getType()->getCanonicalType());
      if (!withCaptures) return funcTy;
      ACE->getCaptureInfo().getLocalCaptures(nullptr, captures);
      return getFunctionTypeWithCaptures(funcTy, captures, ACE->getParent());
    }

    FuncDecl *func = cast<FuncDecl>(vd);
    auto funcTy = cast<AnyFunctionType>(func->getType()->getCanonicalType());
    funcTy = cast<AnyFunctionType>(replaceDynamicSelfWithSelf(funcTy));
    if (!withCaptures) return funcTy;
    func->getLocalCaptures(captures);
    return getFunctionTypeWithCaptures(funcTy, captures,
                                       func->getDeclContext());
  }

  case SILDeclRef::Kind::Allocator:
  case SILDeclRef::Kind::EnumElement:
    return cast<AnyFunctionType>(vd->getType()->getCanonicalType());
  
  case SILDeclRef::Kind::Initializer:
    return cast<AnyFunctionType>(cast<ConstructorDecl>(vd)
                                   ->getInitializerType()->getCanonicalType());
  
  case SILDeclRef::Kind::Destroyer:
  case SILDeclRef::Kind::Deallocator:
    return getDestructorType(cast<DestructorDecl>(vd), 
                             c.kind == SILDeclRef::Kind::Deallocator,
                             Context,
                             c.isForeign);
  
  case SILDeclRef::Kind::GlobalAccessor: {
    VarDecl *var = cast<VarDecl>(vd);
    assert(var->hasStorage() && "constant ref to computed global var");
    return getGlobalAccessorType(var->getType()->getCanonicalType());
  }
  case SILDeclRef::Kind::DefaultArgGenerator:
    return getDefaultArgGeneratorType(*this,
                                      cast<AbstractFunctionDecl>(vd),
                                      c.defaultArgIndex, Context);
  case SILDeclRef::Kind::IVarInitializer:
      return getIVarInitDestroyerType(cast<ClassDecl>(vd), c.isForeign,
                                      Context, false);
  case SILDeclRef::Kind::IVarDestroyer:
    return getIVarInitDestroyerType(cast<ClassDecl>(vd), c.isForeign,
                                    Context, true);
  }
}

Type TypeConverter::getLoweredBridgedType(Type t, AbstractCC cc) {  
  switch (cc) {
  case AbstractCC::Freestanding:
  case AbstractCC::Method:
  case AbstractCC::WitnessMethod:
    // No bridging needed for native CCs.
    return t;
  case AbstractCC::C:
  case AbstractCC::ObjCMethod:
    // Map native types back to bridged types.

    // Look through optional types.
    if (auto valueTy = t->getOptionalObjectType()) {
      auto Ty = getLoweredCBridgedType(valueTy);
      return Ty ? OptionalType::get(Ty) : Ty;
    }
    if (auto valueTy = t->getImplicitlyUnwrappedOptionalObjectType()) {
      auto Ty = getLoweredCBridgedType(valueTy);
      return Ty ? ImplicitlyUnwrappedOptionalType::get(Ty) : Ty;
    }
    return getLoweredCBridgedType(t);
  }
};

Type TypeConverter::getLoweredCBridgedType(Type t) {
#define BRIDGE_TYPE(BridgedModule,BridgedType, NativeModule,NativeType, Opt) \
  { auto nativeType = get##NativeType##Type();                               \
    if (nativeType && t->isEqual(nativeType))                                \
      return get##BridgedType##Type();                                       \
  }
#include "swift/SIL/BridgedTypes.def"

  // Class metatypes bridge to ObjC metatypes.
  if (auto metaTy = t->getAs<MetatypeType>()) {
    if (metaTy->getInstanceType()->getClassOrBoundGenericClass()) {
      return MetatypeType::get(metaTy->getInstanceType(),
                               MetatypeRepresentation::ObjC);
    }
  }

  // ObjC-compatible existential metatypes.
  if (auto metaTy = t->getAs<ExistentialMetatypeType>()) {
    if (metaTy->getInstanceType()->isObjCExistentialType()) {
      return ExistentialMetatypeType::get(metaTy->getInstanceType(),
                                          MetatypeRepresentation::ObjC);
    }
  }
  
  if (auto funTy = t->getAs<FunctionType>()) {
    switch (funTy->getRepresentation()) {
    case AnyFunctionType::Representation::Block:
      // Functions that are already represented as blocks don't need bridging.
      return t;
    case AnyFunctionType::Representation::Thin:
      // TODO: Bridge thin functions to C function pointers?
      return t;
    case AnyFunctionType::Representation::Thick:
      // Thick functions (TODO: conditionally) get bridged to blocks.
      return FunctionType::get(funTy->getInput(), funTy->getResult(),
                               funTy->getExtInfo().withRepresentation(
                                        FunctionType::Representation::Block));
    }
  }

  // Array bridging.
  if (auto arrayDecl = Context.getArrayDecl()) {
    if (t->getAnyNominal() == arrayDecl) {
      return getNSArrayType();
    }
  }

  // Dictionary bridging.
  if (auto dictDecl = Context.getDictionaryDecl()) {
    if (t->getAnyNominal() == dictDecl) {
      return getNSDictionaryType();
    }
  }

  return t;
}

SILType TypeConverter::getSubstitutedStorageType(ValueDecl *value,
                                                 Type lvalueType) {
  // The l-value type is the result of applying substitutions to
  // the type-of-reference.  Essentially, we want to apply those
  // same substitutions to value->getType().

  // Canonicalize and lower the l-value's object type.
  CanType origType;
  if (auto subscript = dyn_cast<SubscriptDecl>(value)) {
    origType = subscript->getElementType()->getCanonicalType();
  } else {
    origType = value->getType()->getCanonicalType();
  }

  CanType substType
    = cast<LValueType>(lvalueType->getCanonicalType()).getObjectType();
  SILType silSubstType
    = getLoweredType(AbstractionPattern(origType), substType).getAddressType();
  substType = silSubstType.getSwiftRValueType();

  // Fast path: if the unsubstituted type from the variable equals the
  // substituted type from the l-value, there's nothing to do.
  if (origType == substType)
    return silSubstType;

  // Type substitution preserves structural type structure, and the
  // type-of-reference is only different in the outermost structural
  // types.  So, basically, we just need to undo the changes made by
  // getTypeOfReference and then reapply them on the substituted type.

  // The only really significant manipulation there is with @weak and
  // @unowned.
  if (auto refType = dyn_cast<ReferenceStorageType>(origType)) {
    substType = CanType(ReferenceStorageType::get(substType,
                                                  refType->getOwnership(),
                                                  Context));
    return SILType::getPrimitiveType(substType, SILValueCategory::Address);
  }

  return silSubstType;
}

void TypeConverter::pushGenericContext(GenericSignature *sig) {
  // If the generic signature is empty, this is a no-op.
  if (!sig)
    return;
  
  // GenericFunctionTypes shouldn't nest.
  assert(!GenericArchetypes.hasValue() && "already in generic context?!");
  assert(DependentTypes.empty() && "already in generic context?!");
  
  // Prepare the ArchetypeBuilder with the generic signature.
  GenericArchetypes.emplace(*M.getSwiftModule(), M.getASTContext().Diags);
  if (GenericArchetypes->addGenericSignature(sig))
    llvm_unreachable("error adding generic signature to archetype builder?!");
  GenericArchetypes->assignArchetypes();
}

void TypeConverter::popGenericContext(GenericSignature *sig) {
  // If the generic signature is empty, this is a no-op.
  if (!sig)
    return;

  assert(GenericArchetypes.hasValue() && "not in generic context?!");
  
  // Erase our cached TypeLowering objects and associated mappings for dependent
  // types.
  // Resetting the DependentBPA will deallocate but not run the destructor of
  // the dependent TypeLowerings.
  for (auto &ti : DependentTypes) {
    // Destroy only the unique entries.
    CanType srcType = CanType(ti.first.OrigType);
    CanType mappedType = ti.second->getLoweredType().getSwiftRValueType();
    if (srcType == mappedType || isa<LValueType>(srcType))
      ti.second->~TypeLowering();
  }
  DependentTypes.clear();
  DependentBPA.Reset();
  GenericArchetypes.reset();
}
