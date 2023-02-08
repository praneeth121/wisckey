/******* newdb *******/
/* db_iter.h
 * 08/06/2019
 * by Mian Qin
 */

#ifndef _newdb_db_iter_h_
#define _newdb_db_iter_h_

#include "db_impl.h"
#include "newdb/iterator.h"

namespace newdb {

Iterator *NewDBIterator(DBImpl *db, const ReadOptions &options);
} // end namespace newdb

#endif