/* mapping_table.h
* 07/03/2019
* by Mian Qin
*/

#ifndef   _mapping_table_h_   
#define   _mapping_table_h_   


#include <string>
#include <stdint.h>
#include "phy_kv.h"


namespace newdb {
typedef enum {
    Mem,
    Storage,
    Cuckoo
} MetaType;

class MapIterator {
public:
    MapIterator() {};
    virtual ~MapIterator() {};
    virtual void Seek(std::string &key) = 0;
    virtual void SeekToFirst() = 0;
    virtual void Next() = 0;
    virtual bool Valid() = 0;
    virtual std::string& Key() = 0;
    virtual std::string& Value() = 0;
};

class Map {
public:
    Map() {};
    virtual ~Map() {};

    // define Map interface
    virtual bool lookup(std::string* key, phy_key* val) = 0;
    virtual bool readmodifywrite(std::string* key, phy_key* rd_val, phy_key* wr_val) = 0;
    virtual bool readtestupdate(std::string* key, phy_key* rd_val, phy_key* old_val, phy_key* new_val) = 0;
    virtual void insert(std::string* key, phy_key* val) = 0;
    virtual void update(std::string* key, phy_key* val) = 0;
    virtual void erase(std::string* key) = 0;

    virtual void commit() {};

    virtual MapIterator* NewMapIterator() = 0;
};

Map* NewMemMap();
Map* NewCuckooMap();

} // namespace newdb
#endif