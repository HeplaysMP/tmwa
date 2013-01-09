#include "inter.hpp"

#include <cstdlib>
#include <cstring>

#include <fstream>

#include "../common/cxxstdio.hpp"
#include "../common/db.hpp"
#include "../common/extract.hpp"
#include "../common/lock.hpp"
#include "../common/mmo.hpp"
#include "../common/socket.hpp"
#include "../common/timer.hpp"

#include "char.hpp"
#include "int_party.hpp"
#include "int_storage.hpp"

#define WISDATA_TTL (60*1000)   // Existence time of Wisp/page data (60 seconds)
                                // that is the waiting time of answers of all map-servers
#define WISDELLIST_MAX 256      // Number of elements of Wisp/page data deletion list

char inter_log_filename[1024] = "log/inter.log";

static
char accreg_txt[1024] = "save/accreg.txt";
static
struct dbt *accreg_db = NULL;

struct accreg
{
    int account_id, reg_num;
    struct global_reg reg[ACCOUNT_REG_NUM];
};

int party_share_level = 10;

// 受信パケット長リスト
static
int inter_recv_packet_length[] = {
    -1, -1, 7, -1, -1, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    6, -1, 0, 0, 0, 0, 0, 0, 10, -1, 0, 0, 0, 0, 0, 0,
    72, 6, 52, 14, 10, 29, 6, -1, 34, 0, 0, 0, 0, 0, 0, 0,
    -1, 6, -1, 0, 55, 19, 6, -1, 14, -1, -1, -1, 14, 19, 186, -1,
    5, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    48, 14, -1, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

struct WisData
{
    int id, fd, count, len;
    unsigned long tick;
    unsigned char src[24], dst[24], msg[1024];
};
static
struct dbt *wis_db = NULL;
static
int wis_dellist[WISDELLIST_MAX], wis_delnum;

//--------------------------------------------------------

// アカウント変数を文字列へ変換
static
std::string inter_accreg_tostr(struct accreg *reg)
{
    std::string str STRPRINTF("%d\t", reg->account_id);
    for (int j = 0; j < reg->reg_num; j++)
        str += STRPRINTF("%s,%d ", reg->reg[j].str, reg->reg[j].value);
    return str;
}

// アカウント変数を文字列から変換
static
bool extract(const_string str, struct accreg *reg)
{
    std::vector<struct global_reg> vars;
    if (!extract(str,
                record<'\t'>(
                    &reg->account_id,
                    vrec<' '>(&vars))))
        return false;
    if (reg->account_id <= 0)
        return false;

    if (vars.size() > ACCOUNT_REG_NUM)
        return false;
    std::copy(vars.begin(), vars.end(), reg->reg);
    reg->reg_num = vars.size();
    return true;
}

// アカウント変数の読み込み
static
int inter_accreg_init(void)
{
    int c = 0;

    accreg_db = numdb_init();

    std::ifstream in(accreg_txt);
    if (!in.is_open())
        return 1;
    std::string line;
    while (std::getline(in, line))
    {
        struct accreg *reg;
        CREATE(reg, struct accreg, 1);
        if (!extract(line, reg))
        {
            numdb_insert(accreg_db, reg->account_id, reg);
        }
        else
        {
            PRINTF("inter: accreg: broken data [%s] line %d\n", accreg_txt,
                    c);
            free(reg);
        }
        c++;
    }

    return 0;
}

// アカウント変数のセーブ用
static
void inter_accreg_save_sub(db_key_t, db_val_t data, FILE *fp)
{
    struct accreg *reg = (struct accreg *) data;

    if (reg->reg_num > 0)
    {
        std::string line = inter_accreg_tostr(reg);
        fwrite(line.data(), 1, line.size(), fp);
        fputc('\n', fp);
    }
}

// アカウント変数のセーブ
static
int inter_accreg_save(void)
{
    FILE *fp;
    int lock;

    if ((fp = lock_fopen(accreg_txt, &lock)) == NULL)
    {
        PRINTF("int_accreg: cant write [%s] !!! data is lost !!!\n",
                accreg_txt);
        return 1;
    }
    numdb_foreach(accreg_db, std::bind(inter_accreg_save_sub, ph::_1, ph::_2, fp));
    lock_fclose(fp, accreg_txt, &lock);

    return 0;
}

//--------------------------------------------------------

/*==========================================
 * 設定ファイルを読み込む
 *------------------------------------------
 */
static
int inter_config_read(const char *cfgName)
{
    std::ifstream in(cfgName);
    if (!in.is_open())
    {
        PRINTF("file not found: %s\n", cfgName);
        return 1;
    }

    std::string line;
    while (std::getline(in, line))
    {
        std::string w1, w2;
        if (!split_key_value(line, &w1, &w2))
            continue;

        if (w1 == "storage_txt")
        {
            strzcpy(storage_txt, w2.c_str(), sizeof(storage_txt));
        }
        else if (w1 == "party_txt")
        {
            strzcpy(party_txt, w2.c_str(), sizeof(party_txt));
        }
        else if (w1 == "accreg_txt")
        {
            strzcpy(accreg_txt, w2.c_str(), sizeof(accreg_txt));
        }
        else if (w1 == "party_share_level")
        {
            party_share_level = atoi(w2.c_str());
            if (party_share_level < 0)
                party_share_level = 0;
        }
        else if (w1 == "inter_log_filename")
        {
            strzcpy(inter_log_filename, w2.c_str(), sizeof(inter_log_filename));
        }
        else if (w1 == "import")
        {
            inter_config_read(w2.c_str());
        }
        else
        {
            PRINTF("WARNING: unknown inter config key: %s\n", w1);
        }
    }

    return 0;
}

// セーブ
int inter_save(void)
{
    inter_party_save();
    inter_storage_save();
    inter_accreg_save();

    return 0;
}

// 初期化
int inter_init(const char *file)
{
    inter_config_read(file);

    wis_db = numdb_init();

    inter_party_init();
    inter_storage_init();
    inter_accreg_init();

    return 0;
}

//--------------------------------------------------------
// sended packets to map-server

// GMメッセージ送信
static
void mapif_GMmessage(const uint8_t *mes, int len)
{
    unsigned char buf[len];

    WBUFW(buf, 0) = 0x3800;
    WBUFW(buf, 2) = len;
    memcpy(WBUFP(buf, 4), mes, len - 4);
    mapif_sendall(buf, len);
}

// Wisp/page transmission to all map-server
static
int mapif_wis_message(struct WisData *wd)
{
    unsigned char buf[56 + wd->len];

    WBUFW(buf, 0) = 0x3801;
    WBUFW(buf, 2) = 56 + wd->len;
    WBUFL(buf, 4) = wd->id;
    memcpy(WBUFP(buf, 8), wd->src, 24);
    memcpy(WBUFP(buf, 32), wd->dst, 24);
    memcpy(WBUFP(buf, 56), wd->msg, wd->len);
    wd->count = mapif_sendall(buf, WBUFW(buf, 2));

    return 0;
}

// Wisp/page transmission result to map-server
static
int mapif_wis_end(struct WisData *wd, int flag)
{
    unsigned char buf[27];

    WBUFW(buf, 0) = 0x3802;
    memcpy(WBUFP(buf, 2), wd->src, 24);
    WBUFB(buf, 26) = flag;     // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
    mapif_send(wd->fd, buf, 27);

    return 0;
}

// アカウント変数送信
static
int mapif_account_reg(int fd, const uint8_t *src)
{
    unsigned char buf[RBUFW(src, 2)];

    memcpy(WBUFP(buf, 0), src, RBUFW(src, 2));
    WBUFW(buf, 0) = 0x3804;
    mapif_sendallwos(fd, buf, WBUFW(buf, 2));

    return 0;
}

// アカウント変数要求返信
static
int mapif_account_reg_reply(int fd, int account_id)
{
    struct accreg *reg = (struct accreg *)numdb_search(accreg_db, account_id);

    WFIFOW(fd, 0) = 0x3804;
    WFIFOL(fd, 4) = account_id;
    if (reg == NULL)
    {
        WFIFOW(fd, 2) = 8;
    }
    else
    {
        int j, p;
        for (j = 0, p = 8; j < reg->reg_num; j++, p += 36)
        {
            memcpy(WFIFOP(fd, p), reg->reg[j].str, 32);
            WFIFOL(fd, p + 32) = reg->reg[j].value;
        }
        WFIFOW(fd, 2) = p;
    }
    WFIFOSET(fd, WFIFOW(fd, 2));

    return 0;
}

//--------------------------------------------------------

// Existence check of WISP data
static
void check_ttl_wisdata_sub(db_key_t, db_val_t data, unsigned long tick)
{
    struct WisData *wd = (struct WisData *) data;

    if (DIFF_TICK(tick, wd->tick) > WISDATA_TTL
        && wis_delnum < WISDELLIST_MAX)
        wis_dellist[wis_delnum++] = wd->id;
}

static
int check_ttl_wisdata(void)
{
    unsigned long tick = gettick();
    int i;

    do
    {
        wis_delnum = 0;
        numdb_foreach(wis_db, std::bind(check_ttl_wisdata_sub, ph::_1, ph::_2, tick));
        for (i = 0; i < wis_delnum; i++)
        {
            struct WisData *wd = (struct WisData *)numdb_search(wis_db, wis_dellist[i]);
            PRINTF("inter: wis data id=%d time out : from %s to %s\n",
                    wd->id, wd->src, wd->dst);
            // removed. not send information after a timeout. Just no answer for the player
            //mapif_wis_end(wd, 1); // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
            numdb_erase(wis_db, wd->id);
            free(wd);
        }
    }
    while (wis_delnum >= WISDELLIST_MAX);

    return 0;
}

//--------------------------------------------------------
// received packets from map-server

// GMメッセージ送信
static
int mapif_parse_GMmessage(int fd)
{
    mapif_GMmessage(static_cast<const uint8_t *>(RFIFOP(fd, 4)), RFIFOW(fd, 2));

    return 0;
}

// Wisp/page request to send
static
int mapif_parse_WisRequest(int fd)
{
    struct WisData *wd;
    static int wisid = 0;
    int index;

    if (RFIFOW(fd, 2) - 52 >= sizeof(wd->msg))
    {
        PRINTF("inter: Wis message size too long.\n");
        return 0;
    }
    else if (RFIFOW(fd, 2) - 52 <= 0)
    {                           // normaly, impossible, but who knows...
        PRINTF("inter: Wis message doesn't exist.\n");
        return 0;
    }

    // search if character exists before to ask all map-servers
    if ((index = search_character_index((const char *)RFIFOP(fd, 28))) == -1)
    {
        unsigned char buf[27];
        WBUFW(buf, 0) = 0x3802;
        memcpy(WBUFP(buf, 2), RFIFOP(fd, 4), 24);
        WBUFB(buf, 26) = 1;    // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
        mapif_send(fd, buf, 27);
        // Character exists. So, ask all map-servers
    }
    else
    {
        // to be sure of the correct name, rewrite it
        strzcpy(static_cast<char *>(const_cast<void *>(RFIFOP(fd, 28))), search_character_name(index), 24);
        // if source is destination, don't ask other servers.
        if (strcmp((const char *)RFIFOP(fd, 4), (const char *)RFIFOP(fd, 28)) == 0)
        {
            unsigned char buf[27];
            WBUFW(buf, 0) = 0x3802;
            memcpy(WBUFP(buf, 2), RFIFOP(fd, 4), 24);
            WBUFB(buf, 26) = 1;    // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
            mapif_send(fd, buf, 27);
        }
        else
        {
            CREATE(wd, struct WisData, 1);

            // Whether the failure of previous wisp/page transmission (timeout)
            check_ttl_wisdata();

            wd->id = ++wisid;
            wd->fd = fd;
            wd->len = RFIFOW(fd, 2) - 52;
            memcpy(wd->src, RFIFOP(fd, 4), 24);
            memcpy(wd->dst, RFIFOP(fd, 28), 24);
            memcpy(wd->msg, RFIFOP(fd, 52), wd->len);
            wd->tick = gettick();
            numdb_insert(wis_db, wd->id, wd);
            mapif_wis_message(wd);
        }
    }

    return 0;
}

// Wisp/page transmission result
static
int mapif_parse_WisReply(int fd)
{
    int id = RFIFOL(fd, 2), flag = RFIFOB(fd, 6);
    struct WisData *wd = (struct WisData *)numdb_search(wis_db, id);

    if (wd == NULL)
        return 0;               // This wisp was probably suppress before, because it was timeout of because of target was found on another map-server

    if ((--wd->count) <= 0 || flag != 1)
    {
        mapif_wis_end(wd, flag);   // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
        numdb_erase(wis_db, id);
        free(wd);
    }

    return 0;
}

// Received wisp message from map-server for ALL gm (just copy the message and resends it to ALL map-servers)
static
int mapif_parse_WisToGM(int fd)
{
    unsigned char buf[RFIFOW(fd, 2)];  // 0x3003/0x3803 <packet_len>.w <wispname>.24B <min_gm_level>.w <message>.?B

    memcpy(WBUFP(buf, 0), RFIFOP(fd, 0), RFIFOW(fd, 2));
    WBUFW(buf, 0) = 0x3803;
    mapif_sendall(buf, RFIFOW(fd, 2));

    return 0;
}

// アカウント変数保存要求
static
int mapif_parse_AccReg(int fd)
{
    int j, p;
    struct accreg *reg = (struct accreg*)numdb_search(accreg_db, (numdb_key_t)RFIFOL(fd, 4));

    if (reg == NULL)
    {
        CREATE(reg, struct accreg, 1);
        reg->account_id = RFIFOL(fd, 4);
        numdb_insert(accreg_db, (numdb_key_t)RFIFOL(fd, 4), reg);
    }

    for (j = 0, p = 8; j < ACCOUNT_REG_NUM && p < RFIFOW(fd, 2);
         j++, p += 36)
    {
        memcpy(reg->reg[j].str, RFIFOP(fd, p), 32);
        reg->reg[j].value = RFIFOL(fd, p + 32);
    }
    reg->reg_num = j;

    // 他のMAPサーバーに送信
    mapif_account_reg(fd, static_cast<const uint8_t *>(RFIFOP(fd, 0)));

    return 0;
}

// アカウント変数送信要求
static
int mapif_parse_AccRegRequest(int fd)
{
    return mapif_account_reg_reply(fd, RFIFOL(fd, 2));
}

//--------------------------------------------------------

// map server からの通信（１パケットのみ解析すること）
// エラーなら0(false)、処理できたなら1、
// パケット長が足りなければ2をかえさなければならない
int inter_parse_frommap(int fd)
{
    int cmd = RFIFOW(fd, 0);
    int len = 0;

    // inter鯖管轄かを調べる
    if (cmd < 0x3000
        || cmd >=
        0x3000 +
        (sizeof(inter_recv_packet_length) /
         sizeof(inter_recv_packet_length[0])))
        return 0;

    // パケット長を調べる
    if ((len =
         inter_check_length(fd,
                             inter_recv_packet_length[cmd - 0x3000])) == 0)
        return 2;

    switch (cmd)
    {
        case 0x3000:
            mapif_parse_GMmessage(fd);
            break;
        case 0x3001:
            mapif_parse_WisRequest(fd);
            break;
        case 0x3002:
            mapif_parse_WisReply(fd);
            break;
        case 0x3003:
            mapif_parse_WisToGM(fd);
            break;
        case 0x3004:
            mapif_parse_AccReg(fd);
            break;
        case 0x3005:
            mapif_parse_AccRegRequest(fd);
            break;
        default:
            if (inter_party_parse_frommap(fd))
                break;
            if (inter_storage_parse_frommap(fd))
                break;
            return 0;
    }
    RFIFOSKIP(fd, len);

    return 1;
}

// RFIFOのパケット長確認
// 必要パケット長があればパケット長、まだ足りなければ0
int inter_check_length(int fd, int length)
{
    if (length == -1)
    {                           // 可変パケット長
        if (RFIFOREST(fd) < 4) // パケット長が未着
            return 0;
        length = RFIFOW(fd, 2);
    }

    if (RFIFOREST(fd) < length)    // パケットが未着
        return 0;

    return length;
}