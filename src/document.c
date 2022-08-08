
#include "document.h"
#include "forward_index.h"
#include "numeric_filter.h"
#include "numeric_index.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include "util/mempool.h"
#include "spec.h"
#include "tokenize.h"
#include "util/logging.h"
#include "rmalloc.h"
#include "indexer.h"
#include "tag_index.h"
#include "aggregate/expr/expression.h"
#include "rmutil/rm_assert.h"

#include <string.h>
#include <inttypes.h>

///////////////////////////////////////////////////////////////////////////////////////////////

bool AddDocumentCtx::SetDocument(IndexSpec *sp, Document *doc, size_t oldFieldCount) {
  stateFlags &= ~ACTX_F_INDEXABLES;
  stateFlags &= ~ACTX_F_TEXTINDEXED;
  stateFlags &= ~ACTX_F_OTHERINDEXED;

  if (oldFieldCount < doc->numFields) {
    // Pre-allocate the field specs
    fspecs = rm_realloc(fspecs, sizeof(*fspecs) * doc->numFields);
    fdatas = rm_realloc(fdatas, sizeof(*fdatas) * doc->numFields);
  }

  for (size_t i = 0; i < doc->numFields; ++i) {
    // zero out field data. We check at the destructor to see if there is any
    // left-over tag data here; if we've realloc'd, then this contains garbage
    fdatas[i].tags = TagIndex::Tags();
  }

  size_t numTextIndexable = 0;
  FieldSpecDedupeArray dedupe;
  int hasTextFields = 0;
  int hasOtherFields = 0;

  for (size_t i = 0; i < doc->numFields; i++) {
    DocumentField *f = doc->fields + i;
    const FieldSpec *fs = sp->GetField(f->name, strlen(f->name));
    if (!fs || !f->text) {
      fspecs[i].name = NULL;
      fspecs[i].types = 0;
      continue;
    }

    fspecs[i] = *fs;
    if (dedupe[fs->index]) {
      status.SetErrorFmt(QUERY_EDUPFIELD, "Tried to insert `%s` twice", fs->name);
      return false;
    }

    dedupe[fs->index] = 1;

    if (fs->IsSortable()) {
      // mark sortable fields to be updated in the state flags
      stateFlags |= ACTX_F_SORTABLES;
    }

    // See what we want the given field indexed as:
    if (!f->indexAs) {
      f->indexAs = fs->types;
    } else {
      // Verify the flags:
      if ((f->indexAs & fs->types) != f->indexAs) {
        status.SetErrorFmt(QUERY_EUNSUPPTYPE,
                               "Tried to index field %s as type not specified in schema", fs->name);
        return false;
      }
    }

    if (fs->IsIndexable()) {
      if (f->indexAs & INDEXFLD_T_FULLTEXT) {
        numTextIndexable++;
        hasTextFields = 1;
      }

      if (f->indexAs != INDEXFLD_T_FULLTEXT) {
        // has non-text but indexable fields
        hasOtherFields = 1;
      }

      if (f->CheckIdx(INDEXFLD_T_GEO)) {
        docFlags = Document_HasOnDemandDeletable;
      }
    }
  }

  if (hasTextFields || hasOtherFields) {
    stateFlags |= ACTX_F_INDEXABLES;
  } else {
    stateFlags &= ~ACTX_F_INDEXABLES;
  }

  if (!hasTextFields) {
    stateFlags |= ACTX_F_TEXTINDEXED;
  } else {
    stateFlags &= ~ACTX_F_TEXTINDEXED;
  }

  if (!hasOtherFields) {
    stateFlags |= ACTX_F_OTHERINDEXED;
  } else {
    stateFlags &= ~ACTX_F_OTHERINDEXED;
  }

  if ((stateFlags & ACTX_F_SORTABLES) && sv == NULL) {
    sv = new RSSortingVector(sp->sortables->len);
  }

  int empty = (sv == NULL) && !hasTextFields && !hasOtherFields;
  if (empty) {
    stateFlags |= ACTX_F_EMPTY;
  }

  if ((options & DOCUMENT_ADD_NOSAVE) == 0 && numTextIndexable &&
      (sp->flags & Index_StoreByteOffsets)) {
    byteOffsets = new RSByteOffsets(numTextIndexable);
    offsetsWriter = *new ByteOffsetWriter();
  }

  Document::Move(doc, doc);
  return true;
}

//---------------------------------------------------------------------------------------------

// Creates a new context used for adding documents. Once created, call
// AddToIndexes on it.
//
// - client is a blocked client which will be used as the context for this
//   operation.
// - sp is the index that this document will be added to
// - base is the document to be index. The context will take ownership of the
//   document's contents (but not the structure itself). Thus, you should not
//   call Document_Free on the document after a successful return of this
//   function.
//
// When done, call delete

AddDocumentCtx::AddDocumentCtx(IndexSpec *sp, Document *b, QueryError *status_) {
  stateFlags = 0;
  status.ClearError();
  totalTokens = 0;
  docFlags = 0;
  client.bc = NULL;
  next = NULL;
  indexer = sp->indexer;
  RS_LOG_ASSERT(sp->indexer, "No indexer");

  // Assign the document:
  if (!SetDocument(sp, b, doc.numFields)) {
    *status_ = status;
    status.detail = NULL;
    throw Error("AddDocumentCtx::SetDocument failed");
  }

  // try to reuse the forward index on recycled contexts
  if (fwIdx) {
    fwIdx->Reset(&doc, sp->flags);
  } else {
    fwIdx = new ForwardIndex(&doc, sp->flags);
  }

  //@@TODO encapsulate within ForwardIndex
  if (sp->smap) {
    // we get a read only copy of the synonym map for accessing in the index thread with out worring
    // about thready safe issues
    fwIdx->smap = sp->smap->GetReadOnlyCopy();
  } else {
    fwIdx->smap = NULL;
  }

  tokenizer = GetTokenizer(b->language, fwIdx->stemmer, sp->stopwords);
  doc.docId = 0;
}

//---------------------------------------------------------------------------------------------

static void doReplyFinish(AddDocumentCtx *aCtx, RedisModuleCtx *ctx) {
  aCtx->donecb(aCtx, ctx, aCtx->donecbData);
  delete aCtx;
}

static int replyCallback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  AddDocumentCtx *aCtx = RedisModule_GetBlockedClientPrivateData(ctx);
  doReplyFinish(aCtx, ctx);
  return REDISMODULE_OK;
}

static void threadCallback(void *p) {
  Document::AddToIndexes(p);
}

//---------------------------------------------------------------------------------------------

// Indicate that processing is finished on the current document

void AddDocumentCtx::Finish() {
  if (stateFlags & ACTX_F_NOBLOCK) {
    doReplyFinish(this, client.sctx->redisCtx);
  } else {
    RedisModule_UnblockClient(client.bc, this);
  }
}

//---------------------------------------------------------------------------------------------

// How many bytes in a document to warrant it being tokenized in a separate thread
#define SELF_EXEC_THRESHOLD 1024

/**
 * Print contents of document to screen
 */

// LCOV_EXCL_START debug
void Document::Dump() const {
  printf("Document Key: %s. ID=%" PRIu64 "\n", RedisModule_StringPtrLen(docKey, NULL),
         docId);
  for (size_t ii = 0; ii < numFields; ++ii) {
    printf("  [%lu]: %s => %s\n", ii, fields[ii].name,
           RedisModule_StringPtrLen(fields[ii].text, NULL));
  }
}
// LCOV_EXCL_STOP

//---------------------------------------------------------------------------------------------

bool AddDocumentCtx::ReplaceMerge(RedisSearchCtx *sctx) {
  /**
   * The REPLACE operation contains fields which must be reindexed. This means
   * that a new document ID needs to be assigned, and as a consequence, all
   * fields must be reindexed.
   */
  // Free the old field data
  size_t oldFieldCount = doc.numFields;

  doc.Clear();
  int rv = doc.LoadSchemaFields(sctx);
  if (rv != REDISMODULE_OK) {
    status.SetError(QUERY_ENODOC, "Could not load existing document");
    donecb(this, sctx->redisCtx, donecbData);
    delete this;
    return true;
  }

  // Keep hold of the new fields.
  doc.MakeStringsOwner();
  SetDocument(sctx->spec, &doc, oldFieldCount);
  return false;
}

//---------------------------------------------------------------------------------------------

bool AddDocumentCtx::handlePartialUpdate(RedisSearchCtx *sctx) {
  // Handle partial update of fields
  if (stateFlags & ACTX_F_INDEXABLES) {
    return ReplaceMerge(sctx);
  } else {
    // No indexable fields are updated, we can just update the metadata.
    // Quick update just updates the score, payload and sortable fields of the document.
    // Thus full-reindexing of the document is not required
    UpdateNoIndex(sctx);
    return true;
  }
}

//---------------------------------------------------------------------------------------------

// At this point the context will take over from the caller, and handle sending
// the replies and so on.

void AddDocumentCtx::Submit(RedisSearchCtx *sctx, uint32_t options) {
  options = options;
  if ((options & DOCUMENT_ADD_PARTIAL) && handlePartialUpdate(sctx)) {
    return;
  }

  // We actually modify (!) the strings in the document, so we always require
  // ownership
  doc.MakeStringsOwner();

  if (IsBlockable()) {
    client.bc = RedisModule_BlockClient(sctx->redisCtx, replyCallback, NULL, NULL, 0);
  } else {
    client.sctx = sctx;
  }

  RS_LOG_ASSERT(client.bc, "No blocked client");
  size_t totalSize = 0;
  for (size_t ii = 0; ii < doc.numFields; ++ii) {
    const DocumentField *ff = doc.fields + ii;
    if (fspecs[ii].name && (ff->indexAs & (INDEXFLD_T_FULLTEXT | INDEXFLD_T_TAG))) {
      size_t n;
      RedisModule_StringPtrLen(doc.fields[ii].text, &n);
      totalSize += n;
    }
  }

  if (totalSize >= SELF_EXEC_THRESHOLD && IsBlockable()) {
    ConcurrentSearch_ThreadPoolRun(threadCallback, this, CONCURRENT_POOL_INDEX);
  } else {
    Document::AddToIndexes(this);
  }
}

//---------------------------------------------------------------------------------------------

// Free AddDocumentCtx. Should be done once AddToIndexes() completes; or
// when the client is unblocked.

AddDocumentCtx::~AddDocumentCtx() {
  // Free preprocessed data; this is the only reliable place to do it
  for (size_t i = 0; i < doc.numFields; ++i) {
    if (IsValid(i) && fspecs[i].IsFieldType(INDEXFLD_T_TAG) && !!fdatas[i].tags) {
      fdatas[i].tags.Clear();
    }
  }

  delete sv;
  delete tokenizer;

  if (oldMd) {
    oldMd->Decref();
    oldMd = NULL;
  }

  offsetsWriter.Cleanup();
  status.ClearError();

  delete fwIdx;

  rm_free(fspecs);
  rm_free(fdatas);
}

//---------------------------------------------------------------------------------------------

bool FieldSpec::FulltextPreprocessor(AddDocumentCtx *aCtx, const DocumentField *field,
    FieldIndexerData *fdata, QueryError *status) const {
  size_t fl;
  const char *c = RedisModule_StringPtrLen(field->text, &fl);
  if (IsSortable()) {
    aCtx->sv->Put(sortIdx, (void *)c, RS_SORTABLE_STR);
  }

  if (IsIndexable()) {
    ForwardIndexTokenizer tokenizer(aCtx->fwIdx, c, &aCtx->offsetsWriter, ftId, ftWeight);

    uint32_t options = TOKENIZE_DEFAULT_OPTIONS;
    if (IsNoStem()) {
      options |= TOKENIZE_NOSTEM;
    }
    if (IsPhonetics()) {
      options |= TOKENIZE_PHONETICS;
    }
    aCtx->tokenizer->Start((char *)c, fl, options);

    Token tok;
    uint32_t newTokPos;
    while (0 != (newTokPos = aCtx->tokenizer->Next(&tok))) {
      tokenizer.tokenize(tok);
    }

    uint32_t lastTokPos = aCtx->tokenizer->lastOffset;
    if (aCtx->byteOffsets != NULL) {
      aCtx->byteOffsets->AddField(ftId, aCtx->totalTokens + 1, lastTokPos);
    }
    aCtx->totalTokens = lastTokPos;
  }

  return true;
}

//---------------------------------------------------------------------------------------------

bool FieldSpec::NumericPreprocessor(AddDocumentCtx *aCtx, const DocumentField *field,
    FieldIndexerData *fdata, QueryError *status) const {
  if (RedisModule_StringToDouble(field->text, &fdata->numeric) == REDISMODULE_ERR) {
    status->SetCode(QUERY_ENOTNUMERIC);
    return false;
  }

  // If this is a sortable numeric value - copy the value to the sorting vector
  if (IsSortable()) {
    aCtx->sv->Put(sortIdx, &fdata->numeric, RS_SORTABLE_NUM);
  }
  return true;
}

//---------------------------------------------------------------------------------------------

bool IndexBulkData::numericIndexer(AddDocumentCtx *aCtx, RedisSearchCtx *ctx,
    const DocumentField *field, const FieldSpec *fs, FieldIndexerData *fdata,
    QueryError *status) {
  NumericRangeTree *rt = indexDatas[INDEXTYPE_TO_POS(INDEXFLD_T_NUMERIC)];
  if (!rt) {
    RedisModuleString *keyName = ctx->spec->GetFormattedKey(*fs, INDEXFLD_T_NUMERIC);
    rt = indexDatas[IXFLDPOS_NUMERIC] =
        OpenNumericIndex(ctx, keyName, &indexKeys[IXFLDPOS_NUMERIC]);
    if (!rt) {
      status->SetError(QUERY_EGENERIC, "Could not open numeric index for indexing");
      return false;
    }
  }
  size_t sz = rt->Add(aCtx->doc.docId, fdata->numeric);
  ctx->spec->stats.invertedSize += sz; // TODO: exact amount
  ctx->spec->stats.numRecords++;
  return true;
}

//---------------------------------------------------------------------------------------------

bool FieldSpec::GeoPreprocessor(AddDocumentCtx *aCtx, const DocumentField *field,
    FieldIndexerData *fdata, QueryError *status) const {
  const char *c = RedisModule_StringPtrLen(field->text, NULL);
  char *pos = strpbrk(c, " ,");
  if (!pos) {
    status->SetCode(QUERY_EGEOFORMAT);
    return false;
  }
  *pos = '\0';
  pos++;
  fdata->geoSlon = c;
  fdata->geoSlat = pos;
  return true;
}

//---------------------------------------------------------------------------------------------

bool IndexBulkData::geoIndexer(AddDocumentCtx *aCtx, RedisSearchCtx *ctx,
    const DocumentField *field, const FieldSpec *fs, FieldIndexerData *fdata,
    QueryError *status) {
  GeoIndex gi(ctx, *fs);
  int rv = gi.AddStrings(aCtx->doc.docId, fdata->geoSlon, fdata->geoSlat);

  if (rv == REDISMODULE_ERR) {
    status->SetError(QUERY_EGENERIC, "Could not index geo value");
    return false;
  }
  return true;
}

//---------------------------------------------------------------------------------------------

bool FieldSpec::TagPreprocessor(AddDocumentCtx *aCtx, const DocumentField *field,
    FieldIndexerData *fdata, QueryError *status) const {
  fdata->tags = TagIndex::Tags(tagSep, tagFlags, field);
  if (!fdata->tags) {
    return true;
  }
  if (IsSortable()) {
    size_t fl;
    const char *c = RedisModule_StringPtrLen(field->text, &fl);
    aCtx->sv->Put(sortIdx, (void *)c, RS_SORTABLE_STR);
  }
  return true;
}

//---------------------------------------------------------------------------------------------

bool IndexBulkData::tagIndexer(AddDocumentCtx *aCtx, RedisSearchCtx *ctx,
    const DocumentField *field, const FieldSpec *fs, FieldIndexerData *fdata,
    QueryError *status) {
  TagIndex *tidx = indexDatas[IXFLDPOS_TAG];
  if (!tidx) {
    RedisModuleString *kname = ctx->spec->GetFormattedKey(*fs, INDEXFLD_T_TAG);
    tidx = indexDatas[IXFLDPOS_TAG] = TagIndex::Open(ctx, kname, 1, &indexKeys[IXFLDPOS_TAG]);
    if (!tidx) {
      status->SetError(QUERY_EGENERIC, "Could not open tag index for indexing");
      return false;
    }
  }

  ctx->spec->stats.invertedSize += tidx->Index(fdata->tags, aCtx->doc.docId);
  ctx->spec->stats.numRecords++;
  return true;
}

//---------------------------------------------------------------------------------------------

bool IndexBulkData::Add(AddDocumentCtx *cur, RedisSearchCtx *sctx, const DocumentField *field,
                       const FieldSpec *fs, FieldIndexerData *fdata, QueryError *status) {
  bool rc = true;
  for (size_t i = 0; i < INDEXFLD_NUM_TYPES && rc; ++i) {
    // see which types are supported in the current field...
    if (field->indexAs & INDEXTYPE_FROM_POS(i)) {
      switch (i) {
        case IXFLDPOS_TAG:
          rc = tagIndexer(cur, sctx, field, fs, fdata, status);
          break;
        case IXFLDPOS_NUMERIC:
          rc = numericIndexer(cur, sctx, field, fs, fdata, status);
          break;
        case IXFLDPOS_GEO:
          rc = geoIndexer(cur, sctx, field, fs, fdata, status);
          break;
        case IXFLDPOS_FULLTEXT:
          break;
        default:
          rc = false;
          status->SetError(QUERY_EINVAL, "BUG: invalid index type");
          break;
      }
    }
  }
  return rc;
}

//---------------------------------------------------------------------------------------------

void IndexBulkData::Cleanup(RedisSearchCtx *sctx) {
  for (size_t i = 0; i < INDEXFLD_NUM_TYPES; ++i) {
    if (indexKeys[i]) {
      RedisModule_CloseKey(indexKeys[i]);
    }
  }
}

//---------------------------------------------------------------------------------------------

/**
 * This function will tokenize the document and add the resultant tokens to
 * the relevant inverted indexes. This function should be called from a
 * worker thread (see ConcurrentSearch functions).
 *
 *
 * When this function completes, it will send the reply to the client and
 * unblock the client passed when the context was first created.
 */

int Document::AddToIndexes(AddDocumentCtx *aCtx) {
  Document *doc = &aCtx->doc;

  for (size_t i = 0; i < doc->numFields; i++) {
    const FieldSpec *fs = aCtx->fspecs + i;
    const DocumentField *ff = doc->fields + i;
    FieldIndexerData *fdata = aCtx->fdatas + i;

    if (fs->name == NULL || ff->indexAs == 0) {
      LG_DEBUG("Skipping field %s not in index!", doc->fields[i].name);
      continue;
    }

    if (ff->CheckIdx(INDEXFLD_T_FULLTEXT)) {
      if (!fs->FulltextPreprocessor(aCtx, &doc->fields[i], fdata, &aCtx->status)) {
        goto cleanup;
      }
    }

    if (ff->CheckIdx(INDEXFLD_T_NUMERIC)) {
      if (!fs->NumericPreprocessor(aCtx, &doc->fields[i], fdata, &aCtx->status)) {
        goto cleanup;
      }
    }

    if (ff->CheckIdx(INDEXFLD_T_GEO)) {
      if (!fs->GeoPreprocessor(aCtx, &doc->fields[i], fdata, &aCtx->status)) {
        goto cleanup;
      }
    }

    if (ff->CheckIdx(INDEXFLD_T_TAG)) {
      if (!fs->TagPreprocessor(aCtx, &doc->fields[i], fdata, &aCtx->status)) {
        goto cleanup;
      }
    }
  }

cleanup:
  aCtx->status.SetCode(QUERY_EGENERIC);
  aCtx->Finish();
  return REDISMODULE_ERR;
}

//---------------------------------------------------------------------------------------------

/* Evaluate an IF expression (e.g. IF "@foo == 'bar'") against a document, by getting the
 * properties from the sorting table or from the hash representation of the document.
 *
 * NOTE: This is disconnected from the document indexing flow, and loads the document and discards
 * of it internally
 *
 * Returns  REDISMODULE_ERR on failure, OK otherwise
 */

static int Document::EvalExpression(RedisSearchCtx *sctx, RedisModuleString *key, const char *expr,
                                    int *result, QueryError *status) {
  int rc = REDISMODULE_ERR;
  const RSDocumentMetadata *dmd = sctx->spec->docs.GetByKey(key);
  if (!dmd) {
    // We don't know the document...
    status->SetError(QUERY_ENODOC, "");
    return REDISMODULE_ERR;
  }

  // Try to parser the expression first, fail if we can't
  try {
    RSExpr e(expr, strlen(expr), status);

    if (status->HasError()) {
      return REDISMODULE_ERR;
    }

    RLookupRow row;
    RSValue rv;
    RLookupLoadOptions *loadopts = NULL;
    ExprEval *evaluator = NULL;
    IndexSpecCache *spcache = sctx->spec->GetSpecCache();
    RLookup lookup_s(spcache);
    if (e.GetLookupKeys(&lookup_s, status) == EXPR_EVAL_ERR) {
      goto done;
    }

    loadopts = new RLookupLoadOptions(sctx, dmd, status);
    if (lookup_s.LoadDocument(&row, loadopts) != REDISMODULE_OK) {
      delete loadopts;
      goto done;
    }

    evaluator = new ExprEval(status, &lookup_s, &row, &e);
    if (evaluator->Eval(&rv) != EXPR_EVAL_OK) {
      delete evaluator;
      goto done;
    }

    *result = rv.BoolTest();
    rv.Clear();
    rc = REDISMODULE_OK;

  // Clean up:
done:
    row.Cleanup();
    return rc;
  } catch (...) {
    return REDISMODULE_ERR;
  }
}

//---------------------------------------------------------------------------------------------

void AddDocumentCtx::UpdateNoIndex(RedisSearchCtx *sctx) {
#define BAIL(s)                                \
  do {                                         \
    status.SetError(QUERY_EGENERIC, s);        \
    donecb(this, sctx->redisCtx, donecbData);  \
    delete this;                               \
  } while (0);

  t_docId docId = sctx->spec->docs.GetId(doc.docKey);
  if (docId == 0) {
    BAIL("Couldn't load old document");
  }
  RSDocumentMetadata *md = sctx->spec->docs.Get(docId);
  if (!md) {
    BAIL("Couldn't load document metadata");
  }

  // Update the score
  md->score = doc.score;
  // Set the payload if needed
  if (doc.payload) {
    sctx->spec->docs.SetPayload(docId, doc.payload);
  }

  if (stateFlags & ACTX_F_SORTABLES) {
    FieldSpecDedupeArray dedupes = {0};
    // Update sortables if needed
    for (int i = 0; i < doc.numFields; i++) {
      DocumentField *f = &doc.fields[i];
      const FieldSpec *fs = sctx->spec->GetField(f->name, strlen(f->name));
      if (fs == NULL || !fs->IsSortable()) {
        continue;
      }

      if (dedupes[fs->index]) {
        BAIL("Requested to index field twice");
      }

      dedupes[fs->index] = 1;

      int idx = sctx->spec->GetFieldSortingIndex(f->name, strlen(f->name));
      if (idx < 0) continue;

      if (!md->sortVector) {
        md->sortVector = new RSSortingVector(sctx->spec->sortables->len);
      }

      if ((fs->options & FieldSpec_Dynamic) == 0) {
        throw Error("Dynamic field cannot use PARTIAL");
      }

      switch (fs->types) {
        case INDEXFLD_T_FULLTEXT:
        case INDEXFLD_T_TAG:
          md->sortVector->Put(idx, (void *)RedisModule_StringPtrLen(f->text, NULL),
                              RS_SORTABLE_STR);
          break;
        case INDEXFLD_T_NUMERIC: {
          double numval;
          if (RedisModule_StringToDouble(f->text, &numval) == REDISMODULE_ERR) {
            BAIL("Could not parse numeric index value");
          }
          md->sortVector->Put(idx, &numval, RS_SORTABLE_NUM);
          break;
        }
        default:
          BAIL("Unsupported sortable type");
          break;
      }
    }
  }
}

//---------------------------------------------------------------------------------------------

DocumentField *Document::GetField(const char *fieldName) {
  if (!fieldName) return NULL;

  for (int i = 0; i < numFields; i++) {
    if (!strcasecmp(fields[i].name, fieldName)) {
      return &fields[i];
    }
  }
  return NULL;
}

///////////////////////////////////////////////////////////////////////////////////////////////
