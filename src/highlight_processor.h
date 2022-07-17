#include "result_processor.h"

///////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Common parameters passed around for highlighting one or more fields within
 * a document. This structure exists to avoid passing these four parameters
 * discreetly (as we did in previous versiosn)
 */
struct hlpDocContext {
  // Byte offsets, byte-wise
  const RSByteOffsets *byteOffsets;

  // Index result, which contains the term offsets (word-wise)
  const IndexResult *indexResult;

  // Array used for in/out when writing fields. Optimization cache
  Array<iovec *> iovsArr;

  RLookupRow *row;

  hlpDocContext(RSByteOffsets *byteOffsets, IndexResult *indexResult, RLookupRow *row) :
    byteOffsets(byteOffsets), indexResult(indexResult), row(row) {}
};

//---------------------------------------------------------------------------------------------

struct Highlighter : public ResultProcessor {
  Highlighter(const RSSearchOptions *searchopts, const FieldList *fields, const RLookup *lookup);
  virtual ~Highlighter();

  int fragmentizeOptions;
  const FieldList *fields;
  const RLookup *lookup;

  const IndexResult *getIndexResult(t_docId docId);

  void processField(hlpDocContext *docParams, ReturnedField *spec);

  virtual int Next(SearchResult *res);
};

///////////////////////////////////////////////////////////////////////////////////////////////