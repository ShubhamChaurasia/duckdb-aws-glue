#include "catalog/glue_transaction_manager.hpp"
#include "catalog/glue_catalog.hpp"

#include "duckdb/main/attached_database.hpp"

namespace duckdb {

GlueTransactionManager::GlueTransactionManager(AttachedDatabase &db_p, GlueCatalog &glue_catalog)
    : TransactionManager(db_p), glue_catalog(glue_catalog) {
}

Transaction &GlueTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<GlueTransaction>(glue_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData GlueTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &glue_transaction = transaction.Cast<GlueTransaction>();
	glue_transaction.Commit();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void GlueTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &glue_transaction = transaction.Cast<GlueTransaction>();
	glue_transaction.Rollback();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void GlueTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// nothing to checkpoint, all changes are already in Glue
}

} // namespace duckdb
