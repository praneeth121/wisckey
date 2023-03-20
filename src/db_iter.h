/******* wisckey *******/
/* db_iter.h
 * 08/06/2019
 * by Mian Qin
 */

#ifndef _wisckey_db_iter_h_
#define _wisckey_db_iter_h_

#include "db_impl.h"
#include "wisckey/iterator.h"

namespace wisckey {

Iterator *NewDBIterator(DBImpl *db, const ReadOptions &options);
} // end namespace wisckey

#endif