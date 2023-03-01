/* phy_kv.h
* 07/06/2019
* by Mian Qin
*/

#ifndef   _phy_kv_h_   
#define   _phy_kv_h_   

#include <string>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define FIXED_KEY_LEN sizeof(uint64_t)

namespace newdb {
class phy_key
{
public:
	uint64_t phykey;
    //char c_key[FIXED_KEY_LEN];
    //std::string c_key;

	phy_key(){}

    phy_key(uint64_t pk) : phykey(pk) {};

	phy_key(uint64_t slab_id, uint64_t seq) {
		uint64_t tmp = slab_id;
		phykey = (seq & 0x00FFFFFFFFFFFFFF) | (tmp << 56);
        // tmp = htobe64(phykey);
        // //sprintf(c_key, "%*.s", FIXED_KEY_LEN, (char*)&tmp);
        // //memcpy(c_key, (char*)&tmp, FIXED_KEY_LEN);
        // c_key.append((char*)&tmp, FIXED_KEY_LEN);
	}

    phy_key(char *key, int len) {
        std::string c_key;
        c_key.append(key, len);
        decode(&c_key);
    }

    void decode(std::string *s_key) {
        //memcpy(c_key, (void *)s_key->c_str(), FIXED_KEY_LEN);
        std::string c_key;
        c_key.clear();
        c_key.append((char*)s_key->c_str(), FIXED_KEY_LEN);
        // uint64_t *tmp = (uint64_t *)c_key.c_str();
        // phykey = be64toh(*tmp);
        phykey = *(uint64_t *)c_key.c_str();
    }

	int get_slab_id() {
		return ((phykey >> 56) & 0xff);
	}

    uint8_t get_klen() {
		return FIXED_KEY_LEN;
	}

	uint64_t get_seq() {
		return (phykey & 0x00FFFFFFFFFFFFFF);
	}

    const uint64_t get_phykey() const {return phykey;}

    const char* c_str() {return (char*)&phykey;}

    std::string ToString() {
        //uint64_t tmp = htobe64(phykey);
        return std::string((char*)&phykey, FIXED_KEY_LEN);
    }
};
inline bool operator==(const phy_key& lk, const phy_key& hk){
    return lk.phykey == hk.phykey;
}
inline bool operator!=(const phy_key& lk, const phy_key& hk){
    return lk.phykey != hk.phykey;
}

class phy_val {
public:
    char *c_val;
    uint32_t val_len;
    uint32_t actual_len;
    phy_val(){};
    phy_val (char *c, int len):
        c_val(c), val_len(len), actual_len(0) {};
    phy_val (phy_val* pval) :
        c_val(pval->c_val), val_len(pval->val_len), actual_len(pval->actual_len) {};
};

enum KVD_OPS {
    KVD_STORE,
    KVD_DELETE,
    KVD_GET
};

} // namespace newdb

#endif