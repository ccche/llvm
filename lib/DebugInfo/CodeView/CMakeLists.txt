add_llvm_library(LLVMDebugInfoCodeView
  CodeViewError.cpp
  CVTypeVisitor.cpp
  EnumTables.cpp
  FieldListRecordBuilder.cpp
  Line.cpp
  ListRecordBuilder.cpp
  MemoryTypeTableBuilder.cpp
  MethodListRecordBuilder.cpp
  ModuleSubstream.cpp
  ModuleSubstreamVisitor.cpp
  RecordSerialization.cpp
  SymbolDumper.cpp
  TypeDeserializer.cpp
  TypeDumper.cpp
  TypeRecord.cpp
  TypeRecordBuilder.cpp
  TypeStreamMerger.cpp
  TypeTableBuilder.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/DebugInfo/CodeView
  )
