#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "catalog/glue_transaction.hpp"

namespace duckdb {
class GlueCatalog;

class GlueTransactionManager : public TransactionManager {
public:
	GlueTransactionManager(AttachedDatabase &db_p, GlueCatalog &glue_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;

	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	GlueCatalog &glue_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<GlueTransaction>> transactions;
};

} // namespace duckdb
