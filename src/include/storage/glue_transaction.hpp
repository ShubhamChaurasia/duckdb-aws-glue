#pragma once

#include "duckdb/transaction/transaction.hpp"

#include "storage/glue_metadata_cache.hpp"

namespace duckdb {
class GlueCatalog;

enum class GlueTransactionState { TRANSACTION_NOT_YET_STARTED, TRANSACTION_STARTED, TRANSACTION_FINISHED };

//! Glue has no transactional semantics in this extension: every DDL statement is executed against the Glue API
//! immediately. The transaction only exists because every attached database needs one.
class GlueTransaction : public Transaction {
public:
	GlueTransaction(GlueCatalog &glue_catalog, TransactionManager &manager, ClientContext &context);
	~GlueTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	static GlueTransaction &Get(ClientContext &context, Catalog &catalog);

public:
	GlueCatalog &glue_catalog;
	//! Glue metadata this transaction resolved: kept for the whole statement (or explicit transaction) so every
	//! reference to a table sees the same definition and partitions
	GlueStatementCache statement_cache;

private:
	GlueTransactionState transaction_state;
};

} // namespace duckdb
