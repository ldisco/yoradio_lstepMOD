/*
 * flac_decoder.cpp
 * Java source code from https://www.nayuki.io/page/simple-flac-implementation
 * adapted to ESP32
 *
 * Created on: Jul 03,2020
 * Updated on: Aug 28,2025
 *
 * Author: Wolle
 *
 */
#include "flac_decoder.h"
#include "vector"
using namespace std;

ps_ptr<FLACFrameHeader_t> FLACFrameHeader;
ps_ptr<FLACMetadataBlock_t> FLACMetadataBlock;

vector<uint32_t> s_flacSegmTableVec;
vector<int32_t>  coefs;
vector<uint32_t> s_flacBlockPicItem;
uint64_t         s_flac_bitBuffer = 0;
uint32_t         s_flacBitrate = 0;
uint32_t         s_flacBlockPicLenUntilFrameEnd = 0;
uint32_t         s_flacCurrentFilePos = 0;
uint32_t         s_flacBlockPicPos = 0;
uint32_t         s_flacBlockPicLen = 0;
uint32_t         s_flacAudioDataStart = 0;
int32_t          s_flacRemainBlockPicLen = 0;
const uint16_t   s_flacOutBuffSize = 2048;
uint16_t         s_numOfOutSamples = 0;
uint16_t         s_flacValidSamples = 0;
uint16_t         s_rIndex = 0;
uint16_t         s_offset = 0;
uint8_t          s_flacStatus = 0;
uint8_t*         s_flacInptr;
float            s_flacCompressionRatio = 0;
uint8_t          s_flacBitBufferLen = 0;
bool             s_f_flacParseOgg = false;
bool             s_f_bitReaderError = false;
// Флаг «ошибка bitreader уже залогирована в текущем аварийном эпизоде».
// Нужен, чтобы при деградации FLAC-потока не засыпать Serial тысячами одинаковых строк.
bool             s_f_bitReaderErrorLogged = false;
uint8_t          s_flac_pageSegments = 0;
ps_ptr<char>     s_flacStreamTitle = {};
ps_ptr<char>     s_flacVendorString = {};
bool             s_f_flacNewStreamtitle = false;
bool             s_f_flacFirstCall = true;
bool             s_f_oggWrapper = false;
bool             s_f_lastMetaDataBlock = false;
bool             s_f_flacNewMetadataBlockPicture = false;
uint8_t          s_flacPageNr = 0;
std::vector<ps_ptr<int32_t>> s_samplesBuffer;
uint16_t         s_maxBlocksize = FLAC_MAX_BLOCKSIZE;
int32_t          s_nBytes = 0;
// [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
// H1: у страницы OGG выставлен бит continued (0x01) — первый lace-сегмент этой страницы
// является ХВОСТОМ FLAC-кадра с предыдущей страницы и НЕ содержит sync 0xFFF8.
bool             s_continued_page = false;
// Одноразовый флаг «первый FLAC-кадр после (пере)синхронизации по OggS» — как в эталоне FUSION.
bool             s_f_first_flac_frame = false;
// Длина текущего lace-сегмента (нужна, чтобы целиком выбросить хвостовой сегмент continued-страницы).
uint32_t         s_segmLength = 0;

//----------------------------------------------------------------------------------------------------------------------
//          FLAC INI SECTION
//----------------------------------------------------------------------------------------------------------------------

bool FLACDecoder_AllocateBuffers(void){

    FLACFrameHeader.alloc();
    FLACMetadataBlock.alloc();

    s_samplesBuffer.resize(FLAC_MAX_CHANNELS);
    for (int32_t i = 0; i < FLAC_MAX_CHANNELS; i++){
        s_samplesBuffer[i].alloc(s_maxBlocksize  * sizeof(int32_t));
        if(!s_samplesBuffer[i]){ // ps_ptr<T> sollte operator bool() überladen
            FLAC_LOG_ERROR("not enough memory to allocate flacdecoder buffers");
            s_samplesBuffer.clear(); // Vektor leeren und alle ps_ptr Objekte zerstören
            return false;
        }
    }

    FLACDecoder_ClearBuffer();
    FLACDecoder_setDefaults();
    s_flacPageNr = 0;
    return true;
}
//----------------------------------------------------------------------------------------------------------------------
void FLACDecoder_ClearBuffer(){
    FLACFrameHeader.zero_mem();
    FLACMetadataBlock.zero_mem();

    for (int32_t i = 0; i < FLAC_MAX_CHANNELS; i++){
        s_samplesBuffer[i].zero_mem();
    }

    s_flacSegmTableVec.clear(); s_flacSegmTableVec.shrink_to_fit();
    s_flacStatus = DECODE_FRAME;
    return;
}
//----------------------------------------------------------------------------------------------------------------------
void FLACDecoder_FreeBuffers(){
    FLACFrameHeader.reset();
    FLACMetadataBlock.reset();
    s_flacStreamTitle.reset();
    s_flacVendorString.reset();

    s_samplesBuffer.clear(); s_samplesBuffer.shrink_to_fit();
    coefs.clear(); coefs.shrink_to_fit();
    s_flacSegmTableVec.clear(); s_flacSegmTableVec.shrink_to_fit();
    s_flacBlockPicItem.clear(); s_flacBlockPicItem.shrink_to_fit();
}
//----------------------------------------------------------------------------------------------------------------------
void FLACDecoder_setDefaults(){
    coefs.clear(); coefs.shrink_to_fit();
    s_flacSegmTableVec.clear(); s_flacSegmTableVec.shrink_to_fit();
    s_flacBlockPicItem.clear(); s_flacBlockPicItem.shrink_to_fit();
    s_flac_bitBuffer = 0;
    s_flacBitrate = 0;
    s_flacBlockPicLenUntilFrameEnd = 0;
    s_flacCurrentFilePos = 0;
    s_flacBlockPicPos = 0;
    s_flacBlockPicLen = 0;
    s_flacRemainBlockPicLen = 0;
    s_flacAudioDataStart = 0;
    s_numOfOutSamples = 0;
    s_offset = 0;
    s_flacValidSamples = 0;
    s_rIndex = 0;
    s_flacStatus = DECODE_FRAME;
    s_flacCompressionRatio = 0;
    s_flacBitBufferLen = 0;
    s_flac_pageSegments = 0;
    s_f_flacNewStreamtitle = false;
    s_f_flacFirstCall = true;
    s_f_oggWrapper = false;
    s_f_lastMetaDataBlock = false;
    s_f_flacNewMetadataBlockPicture = false;
    s_f_flacParseOgg = false;
    s_f_bitReaderError = false;
    // Сбрасываем одноразовый флаг логирования при полном сбросе состояния декодера.
    // Это позволяет в следующем независимом эпизоде снова вывести одну диагностическую строку.
    s_f_bitReaderErrorLogged = false;
    s_nBytes = 0;
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    s_continued_page = false;
    s_f_first_flac_frame = false; // будет взведён заново при обнаружении «OggS» в FLACDecode()
    s_segmLength = 0;
}
//----------------------------------------------------------------------------------------------------------------------
//            B I T R E A D E R
//----------------------------------------------------------------------------------------------------------------------
const uint32_t mask[] = {0x00000000, 0x00000001, 0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f,
                         0x0000007f, 0x000000ff, 0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff, 0x00001fff,
                         0x00003fff, 0x00007fff, 0x0000ffff, 0x0001ffff, 0x0003ffff, 0x0007ffff, 0x000fffff,
                         0x001fffff, 0x003fffff, 0x007fffff, 0x00ffffff, 0x01ffffff, 0x03ffffff, 0x07ffffff,
                         0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff, 0xffffffff};

uint32_t readUint(uint8_t nBits, int32_t *bytesLeft){
    // Если ранее уже зафиксирован underflow bitreader, выходим немедленно.
    // Это жестко обрывает каскад повторных чтений и защищает монитор порта от log-storm.
    if (s_f_bitReaderError) {
        return 0;
    }
    while (s_flacBitBufferLen < nBits){
        uint8_t temp = *(s_flacInptr + s_rIndex);
        s_rIndex++;
        (*bytesLeft)--;
        // [Opus5] OGG-FLAC Sector Progressive (lstepMOD) — H5 / H5b-fix
        // Как Fusion: кадр длиннее lace → на *bytesLeft==-1 снять следующий OggS и продолжить.
        // H5b: НЕ specialIndexOf(...,4) — при baselen==strlen цикл не крутится и всегда
        // «находит» OggS → ложный FLACparseOGG → "OggS not found" в логе. Только memcmp.
        // Без mid-SUBFRAMES resume и без s_flacOggCrossAt — только честный cross + abs().
        if(*bytesLeft == -1 && s_f_oggWrapper){
            s_rIndex--;
            (*bytesLeft)++; // курсор снова на байте сразу за концом lace (= ожидаемый OggS)
            if(memcmp(s_flacInptr + s_rIndex, "OggS", 4) == 0){
                // parseOGG сам пишет "OggS not found", если заголовок битый; таблицу трогаем
                // только при успехе — иначе не вызовем .back() на пустом/старом состоянии.
                if(FLACparseOGG(s_flacInptr + s_rIndex, bytesLeft) == FLAC_NONE && !s_flacSegmTableVec.empty()){
                    s_segmLength = s_flacSegmTableVec.back();
                    s_flacSegmTableVec.pop_back();
                    s_f_flacParseOgg = false; // mid-frame: не парсить OGG снаружи, пока кадр не закончен
                    s_rIndex += (uint16_t)abs(*bytesLeft); // *bytesLeft отрицателен после parse — только abs
                    continue; // читать payload продолжения, не байт «O» из OggS
                }
            }
            if(!s_f_bitReaderErrorLogged){
                FLAC_LOG_ERROR("error in bitreader");
                s_f_bitReaderErrorLogged = true;
            }
            s_f_bitReaderError = true;
            break;
        }
        // Native (не-OGG) FLAC: отрицательный остаток по-прежнему фатален (наша защита от log-storm).
        // OGG после H5-cross: *bytesLeft < -1 — штатно, не ошибка.
        if(*bytesLeft < 0 && !s_f_oggWrapper){
            if(!s_f_bitReaderErrorLogged){
                FLAC_LOG_ERROR("error in bitreader");
                s_f_bitReaderErrorLogged = true;
            }
            s_f_bitReaderError = true;
            break;
        }
        s_flac_bitBuffer = (s_flac_bitBuffer << 8) | temp;
        s_flacBitBufferLen += 8;
    }
    // Если выйти из цикла из-за ошибки или данных все равно недостаточно — возвращаем 0 без побочных эффектов.
    // Важно: не выполняем вычитание s_flacBitBufferLen -= nBits, чтобы не получить отрицательное переполнение.
    if (s_f_bitReaderError || s_flacBitBufferLen < nBits) {
        return 0;
    }
    s_flacBitBufferLen -= nBits;
    uint32_t result = s_flac_bitBuffer >> s_flacBitBufferLen;
    if (nBits < 32)
        result &= mask[nBits];
    return result;
}

int32_t readSignedInt(int32_t nBits, int32_t* bytesLeft){
    int32_t temp = readUint(nBits, bytesLeft) << (32 - nBits);
    temp = temp >> (32 - nBits); // The C++ compiler uses the sign bit to fill vacated bit positions
    return temp;
}

int64_t readRiceSignedInt(uint8_t param, int32_t* bytesLeft){
    long val = 0;
    // [v0.8.208][КРИТИЧНО] Защита от БЕСКОНЕЧНОГО ЦИКЛА (зависание «намертво» на FLAC-станциях):
    // после установки s_f_bitReaderError функция readUint() мгновенно возвращает 0 (см. строки ~151-153),
    // а для unary-кода Райса 0 означает «читай следующий бит» — цикл никогда не завершался.
    // Задача декодера (приоритет 2, core 1) крутилась без yield → главный цикл/веб (приоритет 1, core 1)
    // замирали навсегда → FREEZE-RECOVERY перезагружал устройство. Лог: «error in bitreader» есть,
    // а «Flac bitreader underflow» уже не печатается. Выходим из цикла при ошибке bitreader —
    // выше по стеку decodeResiduals() вернёт FLAC_ERR и декодер ресинхронизируется (фриз, не зависание).
    while (readUint(1, bytesLeft) == 0){
        if (s_f_bitReaderError) return 0;
        val++;
    }
    val = (val << param) | readUint(param, bytesLeft);
    return (val >> 1) ^ -(val & 1);
}

void alignToByte() {
    s_flacBitBufferLen -= s_flacBitBufferLen % 8;
}
//----------------------------------------------------------------------------------------------------------------------
//              F L A C - D E C O D E R
//----------------------------------------------------------------------------------------------------------------------
void FLACSetRawBlockParams(uint8_t Chans, uint32_t SampRate, uint8_t BPS, uint32_t tsis, uint32_t AuDaLength){
    FLACMetadataBlock->numChannels = Chans;
    FLACMetadataBlock->sampleRate = SampRate;
    FLACMetadataBlock->bitsPerSample = BPS;
    FLACMetadataBlock->totalSamples = tsis;  // total samples in stream
    FLACMetadataBlock->audioDataLength = AuDaLength;
}
//----------------------------------------------------------------------------------------------------------------------
void FLACDecoderReset(){ // set var to default
    FLACDecoder_setDefaults();
    FLACDecoder_ClearBuffer();
}
//----------------------------------------------------------------------------------------------------------------------
// [v0.8.277] Сброс «зависшего» OGG-аудиопакета после FLAC_ERR / ресинхронизации по OggS.
// Без этого s_nBytes остаётся > 0, и декодер продолжает читать не с границы lace-пакета —
// отсюда каскад «unimplemented reserved subtype», «decoder asynchron» (Sector SPACE и др.).
void FLACDecoderAbortOggPacket(){
    s_nBytes = 0;
    s_flacStatus = DECODE_FRAME;
    s_rIndex = 0;
    s_offset = 0;
    s_flacBitBufferLen = 0;
    s_flac_bitBuffer = 0;
    s_f_bitReaderError = false;
    s_f_bitReaderErrorLogged = false;
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    // H3. На audio-странице (pageNr >= 2) после ошибки декода inbuf уже НЕ на границе сегмента.
    // Старое поведение: table ещё не пуста → s_f_flacParseOgg=false → следующий вызов берёт
    // следующий lace с ТЕМ ЖЕ inbuf → underflow / «OggS not found» каскадом и фейковый BITRATE ~2.x.
    // Фикс: слить остаток lace-таблицы и принудительно парсить заголовок следующей OGG-страницы.
    // Страницы 0/1 (метаданные): как раньше — следующий сегмент той же страницы, если таблица не пуста.
    if(s_flacPageNr >= 2){
        s_flacSegmTableVec.clear();
        s_f_flacParseOgg = true;
    } else {
        s_f_flacParseOgg = (s_flacSegmTableVec.size() == 0);
    }
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    // После любой ресинхронизации следующий аудио-lace может быть ХВОСТОМ кадра (continued-страница) —
    // взводим одноразовый флаг, иначе хвост опять уехал бы в разбор как «новый кадр».
    // Если страница не continued, флаг ни на что не влияет.
    s_f_first_flac_frame = true;
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FLACFindSyncWord(unsigned char *buf, int32_t nBytes) {

    int32_t i = FLAC_specialIndexOf(buf, "OggS", nBytes);
    if(i == 0) {
        // Найдена корректная синхронизация Ogg-страницы: снимаем аварийные флаги bitreader
        // и сбрасываем незавершённый аудиопакет (иначе декодер начнёт с середины lace).
        FLACDecoderAbortOggPacket();
        return 0;
    }  // flag has ogg wrapper

    if(s_f_oggWrapper && i > 0){
        // При успешной ресинхронизации внутри уже активного Ogg-потока — тот же сброс пакета.
        FLACDecoderAbortOggPacket();
        return i;
    }
    else{
        // [v0.8.253] Перенос надёжного поиска синхро-слова из референсного мода yoRadio v0.9.720m (Maleksm).
        //
        // ПРИЧИНА: старая проверка ниже (закомментирована) сверяла только 2 байта заголовка
        // (0xFF + 0xF8-0xFB) и СРАЗУ считала это настоящим началом кадра. Но сжатые FLAC-данные
        // (Rice/Huffman-остатки) — это практически случайный байтовый мусор, и комбинация "0xFF +
        // 0xF8-FB" встречается там случайно примерно раз на ~1000 байт. При любой ресинхронизации
        // (напр. на "шумных"/высокобитрейтных 24-bit потоках вроде "Радио Сектор" при нехватке
        // сетевой пропускной способности) декодер хватался за ПЕРВОЕ такое случайное совпадение
        // внутри мусорных данных и начинал декодировать случайные байты как настоящий заголовок —
        // отсюда каскад ошибок "bits per sample >24/<8", "unknown/reserved channel assignment",
        // "unimplemented reserved subtype", "wrong rice partition number", "blockSizeOut too big",
        // "bitreader underflow" → "decoder asynchron" → и по кругу (ложный синхрон снова находил
        // ложный синхрон), плюс скачущий битрейт (пересчитывается на каждом таком ложном ресинхроне).
        //
        // ФИКС: как в референсе — принимаем 14-битный sync-код ТОЛЬКО если ВСЕ поля заголовка кадра
        // проходят проверку на допустимость (reserved-бит, коды блок-сайза/сэмплрейта/каналов/bps,
        // корректный UTF-8 номер кадра) И совпадает CRC-8 всего заголовка (полином 0x07, как в
        // спецификации FLAC). Шанс случайного ложного совпадения падает с ~1/1000 до практически
        // нулевого (нужно совпадение CRC-8 — 1/256 — ПОСЛЕ того как уже прошли все проверки полей).

        // Таблица CRC-8 FLAC (полином 0x07, начальное значение 0) — как в заголовке каждого frame FLAC.
        static const uint8_t FLAC_CRC8_TABLE[256] = {
            0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D, 0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
            0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD, 0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
            0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA, 0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
            0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A, 0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
            0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4, 0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
            0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44, 0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
            0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63, 0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
            0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83, 0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3};

        // Разбор укороченного UTF-8 (номер кадра/сэмпла в заголовке FLAC-кадра).
        // Возвращает число занятых байт (1..7) или 0, если последовательность некорректна.
        auto parseFlacUtf8 = [&](const uint8_t* p, uint64_t& value) -> int {
            value = 0;
            uint8_t c = p[0];

            if ((c & 0x80) == 0) { value = c; return 1; }      // один байт: 0xxxxxxx
            if ((c & 0xC0) == 0x80) { return 0; }               // 10xxxxxx как первый байт — невалидно

            int     len  = 0;
            uint8_t mask = 0;
            if      ((c & 0xE0) == 0xC0) { len = 2; mask = 0x1F; } // 110xxxxx
            else if ((c & 0xF0) == 0xE0) { len = 3; mask = 0x0F; } // 1110xxxx
            else if ((c & 0xF8) == 0xF0) { len = 4; mask = 0x07; } // 11110xxx
            else if ((c & 0xFC) == 0xF8) { len = 5; mask = 0x03; } // 111110xx
            else if ((c & 0xFE) == 0xFC) { len = 6; mask = 0x01; } // 1111110x
            else if (c == 0xFE)          { len = 7; mask = 0x00; } // 11111110
            else return 0; // невалидный стартовый байт

            value = c & mask;
            for (int k = 1; k < len; k++) {
                uint8_t b = p[k];
                if ((b & 0xC0) != 0x80) return 0; // continuation-байт обязан быть 10xxxxxx
                value = (value << 6) | (b & 0x3F);
            }
            if (value >= (1ULL << 36)) return 0; // лимит FLAC — не более 36 бит
            return len;
        };

        // Проверка CRC-8 всего заголовка кадра (последний байт заголовка — ожидаемая контрольная сумма).
        auto checkHeaderCRC8 = [&](const uint8_t* hdr, int header_len) -> bool {
            if (header_len < 2) return false;
            uint8_t expected = hdr[header_len - 1];
            uint8_t crc = 0;
            for (int k = 0; k < header_len - 1; k++) { crc = FLAC_CRC8_TABLE[crc ^ hdr[k]]; }
            return crc == expected;
        };

        /* find byte-aligned sync code - need 14 matching bits, затем полная валидация заголовка */
        for (i = 0; i < nBytes - 16; i++) { // запас 16 байт на полный заголовок с учётом доп. полей
            // 1. Sync-код (14 бит): 0xFFF8..0xFFFB в первом слове.
            if (buf[i] != 0xFF || (buf[i + 1] & 0xFC) != 0xF8) continue;

            // Если предыдущий байт тоже 0xFF — это, скорее всего, «расщеплённый» ложный сдвиг синхронизации.
            if (i > 0 && buf[i - 1] == 0xFF) continue;

            // 2. Разбор полей заголовка (2-й и 3-й байты после sync-кода).
            uint8_t b2 = buf[i + 2];
            uint8_t b3 = buf[i + 3];

            uint8_t blocksize_code  = (b2 >> 4) & 0x0F;
            uint8_t samplerate_code =  b2       & 0x0F;
            uint8_t channel_assign  = (b3 >> 4) & 0x0F;
            uint8_t bps_code        = (b3 >> 1) & 0x07;
            uint8_t reserved_bit    =  b3       & 0x01;

            // 3. Проверка допустимости значений (reserved/невозможные коды отбраковываем сразу).
            if (reserved_bit) continue;
            if (blocksize_code == 0) continue;
            if (samplerate_code == 0x0F) continue;
            if (channel_assign > 10) continue;
            if (bps_code == 0 || bps_code == 7) continue;

            // 4. Разбор UTF-8 номера кадра/сэмпла.
            uint64_t frameNum;
            int utf8_len = parseFlacUtf8(&buf[i + 4], frameNum);
            (void)frameNum; // номер кадра здесь не нужен, важен только факт корректного разбора UTF-8
            if (utf8_len == 0) continue;

            // 5. Вычисляем полную длину заголовка (sync+поля(2) + UTF8 + доп.байты blocksize/samplerate + CRC-8).
            int header_len = 4 + utf8_len;
            if      (blocksize_code == 6) header_len += 1; // доп. 8 бит blocksize-1
            else if (blocksize_code == 7) header_len += 2; // доп. 16 бит blocksize-1
            if      (samplerate_code == 12) header_len += 1; // доп. 8 бит, кГц
            else if (samplerate_code == 13) header_len += 2; // доп. 16 бит, Гц
            else if (samplerate_code == 14) header_len += 2; // доп. 16 бит, десятки Гц
            header_len += 1; // байт CRC-8

            if (i + header_len > nBytes) continue; // заголовок не помещается целиком в буфер — не проверить

            // 6. Финальная проверка — CRC-8 всего заголовка должен совпасть.
            if (!checkHeaderCRC8(&buf[i], header_len)) continue;

            // Синхронизация подтверждена всеми проверками — это настоящий кадр.
            if(i) FLACDecoderReset();
            return i;
        }
        // Старая (ненадёжная) проверка — оставлена в истории коммита для справки, заменена выше:
        // for (i = 0; i < nBytes - 1; i++) {
        //     if ((buf[i + 0] & 0xFF) == 0xFF  && (buf[i + 1] & 0xFC) == 0xF8) {
        //         if(i) FLACDecoderReset();
        //         return i;
        //     }
        // }
    }
    return -1;
}
//----------------------------------------------------------------------------------------------------------------------
boolean FLACFindMagicWord(unsigned char* buf, int32_t nBytes){
    int32_t idx = FLAC_specialIndexOf(buf, "fLaC", nBytes);
    if(idx >0){ // Metadatablock follows
        idx += 4;
        boolean lmdbf = ((buf[idx + 1] & 0x80) == 0x80); // Last-metadata-block flag
        uint8_t bt = (buf[idx + 1] & 0x7F); // block type
        uint32_t lomd = (buf[idx + 2] << 16) + (buf[idx + 3] << 8) + buf[idx + 4]; // Length of metadata to follow

        (void)lmdbf; (void)bt; (void)lomd;
        // FLAC_LOG_INFO("Last-metadata-block flag: %d", lmdbf);
        // FLAC_LOG_INFO("block type: %d", bt);
        // FLAC_LOG_INFO("Length (in bytes) of metadata to follow: %d", lomd);
        return true;
    }
    return false;
}
//----------------------------------------------------------------------------------------------------------------------
char* FLACgetStreamTitle(){
    if(s_f_flacNewStreamtitle){
        s_f_flacNewStreamtitle = false;
        return s_flacStreamTitle.get();
    }
    return NULL;
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FLACparseOGG(uint8_t *inbuf, int32_t *bytesLeft){  // reference https://www.xiph.org/ogg/doc/rfc3533.txt

    s_f_flacParseOgg = false;
    int32_t idx = FLAC_specialIndexOf(inbuf, "OggS", 6);
    if(idx != 0){FLAC_LOG_ERROR("Flac decoder asyncron, \"OggS\" not found"); return FLAC_ERR;}

    uint8_t  version            = *(inbuf +  4); (void) version;
    uint8_t  headerType         = *(inbuf +  5); (void) headerType;
    uint64_t granulePosition    = (uint64_t)*(inbuf + 13) << 56;  // granule_position: an 8 Byte field containing -
             granulePosition   += (uint64_t)*(inbuf + 12) << 48;  // position information. For an audio stream, it MAY
             granulePosition   += (uint64_t)*(inbuf + 11) << 40;  // contain the total number of PCM samples encoded
             granulePosition   += (uint64_t)*(inbuf + 10) << 32;  // after including all frames finished on this page.
             granulePosition   += *(inbuf +  9) << 24;  // This is a hint for the decoder and gives it some timing
             granulePosition   += *(inbuf +  8) << 16;  // and position information. A special value of -1 (in two's
             granulePosition   += *(inbuf +  7) << 8;   // complement) indicates that no packets finish on this page.
             granulePosition   += *(inbuf +  6); (void) granulePosition;
    uint32_t bitstreamSerialNr  = *(inbuf + 17) << 24;  // bitstream_serial_number: a 4 Byte field containing the
             bitstreamSerialNr += *(inbuf + 16) << 16;  // unique serial number by which the logical bitstream
             bitstreamSerialNr += *(inbuf + 15) << 8;   // is identified.
             bitstreamSerialNr += *(inbuf + 14); (void) bitstreamSerialNr;
    uint32_t pageSequenceNr     = *(inbuf + 21) << 24;  // page_sequence_number: a 4 Byte field containing the sequence
             pageSequenceNr    += *(inbuf + 20) << 16;  // number of the page so the decoder can identify page loss
             pageSequenceNr    += *(inbuf + 19) << 8;   // This sequence number is increasing on each logical bitstream
             pageSequenceNr    += *(inbuf + 18); (void) pageSequenceNr;
    uint32_t CRCchecksum        = *(inbuf + 25) << 24;
             CRCchecksum       += *(inbuf + 24) << 16;
             CRCchecksum       += *(inbuf + 23) << 8;
             CRCchecksum       += *(inbuf + 22); (void) CRCchecksum;
    uint8_t  pageSegments       = *(inbuf + 26);        // giving the number of segment entries

    // read the segment table (contains pageSegments bytes),  1...251: Length of the frame in bytes,
    // 255: A second byte is needed.  The total length is first_byte + second byte
    s_flacSegmTableVec.clear();
    s_flacSegmTableVec.shrink_to_fit();
    for(int32_t i = 0; i < pageSegments; i++){
        int32_t n = *(inbuf + 27 + i);
        while(*(inbuf + 27 + i) == 255){
            i++;
            if(i == pageSegments) break;
            n+= *(inbuf + 27 + i);
        }
        s_flacSegmTableVec.insert(s_flacSegmTableVec.begin(), n);
    }
    // for(int32_t i = 0; i< s_flacSegmTableVec.size(); i++){FLAC_LOG_INFO("%i", s_flacSegmTableVec[i]);}

    bool     continuedPage = headerType & 0x01; // set: page contains data of a packet continued from the previous page
    bool     firstPage     = headerType & 0x02; // set: this is the first page of a logical bitstream (bos)
    bool     lastPage      = headerType & 0x04; // set: this is the last page of a logical bitstream (eos)

    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    // Было: (void)continuedPage — флаг читался и выбрасывался, поэтому хвост кадра
    // на continued-странице декодировался как новый кадр → reserved subtype / asynchron.
    s_continued_page = continuedPage;
    (void)lastPage;

    // FLAC_LOG_INFO("firstPage %i, continuedPage %i, lastPage %i", firstPage, continuedPage, lastPage);

    if(firstPage) s_flacPageNr = 0;

    uint32_t headerSize = pageSegments + 27;

    *bytesLeft -= headerSize;
    s_flacCurrentFilePos += headerSize;
    return FLAC_NONE; // no error
}

//----------------------------------------------------------------------------------------------------------------------------------------------------
vector<uint32_t> FLACgetMetadataBlockPicture(){
    if(s_f_flacNewMetadataBlockPicture){
        s_f_flacNewMetadataBlockPicture = false;
        return s_flacBlockPicItem;
    }
    if(s_flacBlockPicItem.size() > 0){
        s_flacBlockPicItem.clear();
        s_flacBlockPicItem.shrink_to_fit();
    }
    return s_flacBlockPicItem;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int32_t parseFlacFirstPacket(uint8_t *inbuf, int16_t nBytes){ // 4.2.2. Identification header   https://xiph.org/flac/ogg_mapping.html

    int32_t ret = 0;
    int32_t idx = FLAC_specialIndexOf(inbuf, "fLaC", nBytes);
    //FLAC_LOG_INFO("idx %i, nBytes %i", idx, nBytes);
    if(idx >= 0){ // FLAC signature found
        ret = idx + 4;
    }
    else {
        FLAC_LOG_ERROR("Flac signature \"fLaC\" not found");
        ret = FLAC_ERR;
    }
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int32_t parseMetaDataBlockHeader(uint8_t *inbuf, int16_t nBytes){
    int8_t   ret = FLAC_PARSE_OGG_DONE;
    uint16_t pos = 0;
    int32_t  blockLength = 0;
    uint16_t minBlocksize = 0;
    uint16_t maxBlocksize = 0;
    uint32_t minFrameSize = 0;
    uint32_t maxFrameSize = 0;
    uint32_t sampleRate = 0;
    uint32_t vendorLength = 0;
    uint32_t commemtStringLength = 0;
    uint32_t userCommentListLength = 0;
    uint8_t  nrOfChannels = 0;
    uint8_t  bitsPerSample = 0;
    uint64_t totalSamplesInStream = 0;
    uint8_t  mdBlockHeader = 0;
    uint8_t  blockType = 0;
    uint8_t  bt = 0;
    std::vector<ps_ptr<char>> vb(8); // vorbis comment

    enum {streamInfo, padding, application, seekTable, vorbisComment, cueSheet, picture};


    while(true){
        mdBlockHeader         = *(inbuf + pos);
        s_f_lastMetaDataBlock = mdBlockHeader & 0b10000000; // FLAC_LOG_INFO("lastMdBlockFlag %i", s_f_lastMetaDataBlock);
        blockType             = mdBlockHeader & 0b01111111; // FLAC_LOG_INFO("blockType %i", blockType);

        blockLength        = *(inbuf + pos + 1) << 16;
        blockLength       += *(inbuf + pos + 2) << 8;
        blockLength       += *(inbuf + pos + 3); // FLAC_LOG_INFO("blockLength %i", blockLength);

        nBytes -= 4;
        pos += 4;

        switch(blockType) {
            case 0:
                bt = streamInfo;
                break;
            case 1:
                bt = padding;
            //  FLAC_LOG_ERROR("padding");
                return FLAC_NONE;
                break;
            case 2:
                bt = application;
                FLAC_LOG_ERROR("Flac unimplemented block type: %i", blockType);
                return FLAC_ERR;
                break;
            case 3:
                bt = seekTable;
                FLAC_LOG_ERROR("Flac unimplemented seek table: %i", seekTable);
                return FLAC_ERR;
                break;
            case 4:
                bt = vorbisComment;
                break;
            case 5:
                bt = cueSheet;
                FLAC_LOG_ERROR("Flac unimplemented cue sheet: %i", cueSheet);
                return FLAC_ERR;
                break;
            case 6:
                bt = picture;
                break;
            default:
                bt = streamInfo;
                //return ERR_FLAC_UNIMPLEMENTED;
                break;
        }

        switch(bt){
            case streamInfo:
                minBlocksize += *(inbuf + pos + 0) << 8;
                minBlocksize += *(inbuf + pos + 1);
                maxBlocksize += *(inbuf + pos + 2) << 8;
                maxBlocksize += *(inbuf + pos + 3);
                //FLAC_LOG_INFO("minBlocksize %i", minBlocksize);
                //FLAC_LOG_INFO("maxBlocksize %i", maxBlocksize);
                FLACMetadataBlock->minblocksize = minBlocksize;
                FLACMetadataBlock->maxblocksize = maxBlocksize;

                if(maxBlocksize > s_maxBlocksize){FLAC_LOG_ERROR("s_blocksize is too big: %i bytes, max block size: %i", maxBlocksize, s_maxBlocksize); return FLAC_ERR;}

                minFrameSize  = *(inbuf + pos + 4) << 16;
                minFrameSize += *(inbuf + pos + 5) << 8;
                minFrameSize += *(inbuf + pos + 6);
                maxFrameSize  = *(inbuf + pos + 7) << 16;
                maxFrameSize += *(inbuf + pos + 8) << 8;
                maxFrameSize += *(inbuf + pos + 9);
                //FLAC_LOG_INFO("minFrameSize %i", minFrameSize);
                //FLAC_LOG_INFO("maxFrameSize %i", maxFrameSize);
                FLACMetadataBlock->minframesize = minFrameSize;
                FLACMetadataBlock->maxframesize = maxFrameSize;

                sampleRate   =  *(inbuf + pos + 10) << 12;
                sampleRate  +=  *(inbuf + pos + 11) << 4;
                sampleRate  += (*(inbuf + pos + 12) & 0xF0) >> 4;
                //FLAC_LOG_INFO("sampleRate %i", sampleRate);
                FLACMetadataBlock->sampleRate = sampleRate;

                nrOfChannels = ((*(inbuf + pos + 12) & 0x0E) >> 1) + 1;
                //FLAC_LOG_INFO("nrOfChannels %i", nrOfChannels);
                FLACMetadataBlock->numChannels = nrOfChannels;

                // [v0.8.253] ИСПРАВЛЕНО: было "<< 5", должно быть "<< 4".
                // Поле "bits per sample - 1" в STREAMINFO занимает 5 бит: 1 старший бит
                // (младший бит байта pos+12) + 4 младших бита (старшая тетрада байта pos+13).
                // Старший бит этого 5-битного поля должен сдвигаться на позицию бита №4
                // (т.к. младшие 4 бита занимают позиции 0-3), а не №5.
                // Из-за "<< 5" любой поток с реальным bitsPerSample >= 17 (20/24/32 бит)
                // считался на 16 больше нормы (24 bit ошибочно превращался в 40, что и было
                // видно в логе как "bits per sample > 24 not supported, is: 40").
                // Для 8/16-битных потоков баг не проявлялся, т.к. старший бит 5-битного
                // поля там всегда 0 (bitsPerSample-1 < 16). Верная формула (см. также
                // аналогичный, но правильный расчёт для "чистого" FLAC без OGG-обёртки
                // в Audio::read_FLAC_Header(), Audio.cpp) — сдвиг на 4, а не на 5.
                bitsPerSample  =  (*(inbuf + pos + 12) & 0x01) << 4;
                bitsPerSample += ((*(inbuf + pos + 13) & 0xF0) >> 4) + 1;
                FLACMetadataBlock->bitsPerSample = bitsPerSample;
                //FLAC_LOG_INFO("bitsPerSample %i", bitsPerSample);

                totalSamplesInStream  = (uint64_t)(*(inbuf + pos + 13) & 0x0F) << 32;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 14)) << 24;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 15)) << 16;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 16)) << 8;
                totalSamplesInStream += (uint64_t)(*(inbuf + pos + 17));
                //FLAC_LOG_INFO("totalSamplesInStream %lli", totalSamplesInStream);
                FLACMetadataBlock->totalSamples = totalSamplesInStream;

                //FLAC_LOG_INFO("nBytes %i, blockLength %i", nBytes, blockLength);
                pos += blockLength;
                nBytes -= blockLength;
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            case vorbisComment:                                // https://www.xiph.org/vorbis/doc/v-comment.html
                vendorLength  = *(inbuf + pos + 3) << 24;
                vendorLength += *(inbuf + pos + 2) << 16;
                vendorLength += *(inbuf + pos + 1) <<  8;
                vendorLength += *(inbuf + pos + 0);
                if(vendorLength > 1024){
                    FLAC_LOG_INFO("vendorLength > 1024 bytes");
                }
                s_flacVendorString.alloc(vendorLength + 1); s_flacVendorString.clear();
                s_flacVendorString.copy_from((char*)inbuf + pos + 4, vendorLength);
                // FLAC_LOG_VERBOSE("Vendor: %s", s_flacVendorString.c_get());

                pos += 4 + vendorLength;
                userCommentListLength  = *(inbuf + pos + 3) << 24;
                userCommentListLength += *(inbuf + pos + 2) << 16;
                userCommentListLength += *(inbuf + pos + 1) <<  8;
                userCommentListLength += *(inbuf + pos + 0);

                pos += 4;
                commemtStringLength = 0;
                for(int32_t i = 0; i < userCommentListLength; i++){
                    commemtStringLength  = *(inbuf + pos + 3) << 24;
                    commemtStringLength += *(inbuf + pos + 2) << 16;
                    commemtStringLength += *(inbuf + pos + 1) <<  8;
                    commemtStringLength += *(inbuf + pos + 0);

                    if((FLAC_specialIndexOf(inbuf + pos + 4, "TITLE", 6) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "title", 6) == 0)){
                        vb[0].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        // FLAC_LOG_VERBOSE("TITLE: %s", vb[0].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "ARTIST", 7) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "artist", 7) == 0)){
                        vb[1].assign((const char*)(inbuf + pos + 4 + 7), min((uint32_t)127, commemtStringLength - 7));
                        // FLAC_LOG_VERBOSE("ARTIST: %s", vb[1].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "GENRE", 6) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "genre", 6) == 0)){
                        vb[2].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        FLAC_LOG_VERBOSE("GENRE: %s", vb[2].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "ALBUM", 6) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "album", 6) == 0)){
                        vb[3].assign((const char*)(inbuf + pos + 4 + 6), min((uint32_t)127, commemtStringLength - 6));
                        FLAC_LOG_VERBOSE("ALBUM: %s", vb[3].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "COMMENT", 8) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "comment", 8) == 0)){
                        vb[4].assign((const char*)(inbuf + pos + 4 + 8), min((uint32_t)127, commemtStringLength - 8));
                        FLAC_LOG_VERBOSE("COMMENT: %s", vb[4].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "DATE", 5) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "date", 5) == 0)){
                        vb[5].assign((const char*)(inbuf + pos + 4 + 5), min((uint32_t)127, commemtStringLength - 12));
                        FLAC_LOG_VERBOSE("DATE: %s", vb[5].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "TRACKNUMBER", 12) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "tracknumber", 12) == 0)){
                        vb[6].assign((const char*)(inbuf + pos + 4 + 12), min((uint32_t)127, commemtStringLength - 12));
                        FLAC_LOG_VERBOSE("TRACKNUMBER: %s", vb[6].c_get());
                    }
                    if((FLAC_specialIndexOf(inbuf + pos + 4, "METADATA_BLOCK_PICTURE", 23) == 0) || (FLAC_specialIndexOf(inbuf + pos + 4, "metadata_block_picture", 23) == 0)){
                        FLAC_LOG_VERBOSE("METADATA_BLOCK_PICTURE found, commemtStringLength %i", commemtStringLength);
                        s_flacBlockPicLen = commemtStringLength - 23;
                        s_flacBlockPicPos = s_flacCurrentFilePos + pos + 4 + 23;
                        s_flacBlockPicLenUntilFrameEnd = nBytes - (pos + 23);
                        if(s_flacBlockPicLen < s_flacBlockPicLenUntilFrameEnd) s_flacBlockPicLenUntilFrameEnd = s_flacBlockPicLen;
                        s_flacRemainBlockPicLen = s_flacBlockPicLen - s_flacBlockPicLenUntilFrameEnd;
                        //FLAC_LOG_INFO("s_flacBlockPicPos %i, s_flacBlockPicLen %i", s_flacBlockPicPos, s_flacBlockPicLen);
                        //FLAC_LOG_INFO("s_flacBlockPicLenUntilFrameEnd %i, s_flacRemainBlockPicLen %i", s_flacBlockPicLenUntilFrameEnd, s_flacRemainBlockPicLen);
                        if(s_flacRemainBlockPicLen <= 0) s_f_lastMetaDataBlock = true; // exeption:: goto audiopage after commemt if lastMetaDataFlag is not set
                        if(s_flacBlockPicLen){
                            s_flacBlockPicItem.clear();
                            s_flacBlockPicItem.shrink_to_fit();
                            s_flacBlockPicItem.push_back(s_flacBlockPicPos);
                            s_flacBlockPicItem.push_back(s_flacBlockPicLenUntilFrameEnd);
                        }
                    }
                    pos += 4 + commemtStringLength;
                    // FLAC_LOG_VERBOSE("nBytes %i, pos %i, commemtStringLength %i", nBytes, pos, commemtStringLength);
                }
                if(vb[1].valid() && vb[0].valid()){ // artist and title
                    s_flacStreamTitle.assign(vb[1].c_get());
                    s_flacStreamTitle.append(" - ");
                    s_flacStreamTitle.append(vb[0].c_get());
                    s_f_flacNewStreamtitle = true;
                }
                else if(vb[1].valid()){
                    s_flacStreamTitle.assign(vb[1].c_get());
                    s_f_flacNewStreamtitle = true;
                }
                else if(vb[0].valid()){
                    s_flacStreamTitle.assign(vb[0].c_get());
                    s_f_flacNewStreamtitle = true;
                }
                if(!s_flacBlockPicLen && s_flacSegmTableVec.size() == 1) s_f_lastMetaDataBlock = true; // exeption:: goto audiopage after commemt if lastMetaDataFlag is not set
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            case picture:
                if(ret == FLAC_PARSE_OGG_DONE) return ret;
                break;

            default:
                return ret;
                break;
        }
    }
    return 0;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FLACDecode(uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf){ //  MAIN LOOP

    int32_t                ret = 0;
    uint32_t           segmLen = 0;
    static uint32_t segmLenTmp = 0;
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    static uint8_t  pktWaitCnt = 0; // сколько раз ждали, пока пакет целиком окажется в окне

    if(s_f_flacFirstCall){ // determine if ogg or flag
        s_f_flacFirstCall = false;
        s_nBytes = 0;
        segmLenTmp = 0;
        if(FLAC_specialIndexOf(inbuf, "OggS", 5) == 0){
            s_f_oggWrapper = true;
            s_f_flacParseOgg = true;
            // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
            // Взводим одноразовый флаг: подключение к живому потоку почти всегда попадает
            // в середину кадра, первая аудиостраница помечена continued.
            s_f_first_flac_frame = true;
        }
    }

    if(s_f_oggWrapper){

        if(segmLenTmp){ // can't skip more than 16K
            if(segmLenTmp > FLAC_MAX_BLOCKSIZE){
                s_flacCurrentFilePos += FLAC_MAX_BLOCKSIZE;
                *bytesLeft -= FLAC_MAX_BLOCKSIZE;
                segmLenTmp -= FLAC_MAX_BLOCKSIZE;
            }
            else{
                s_flacCurrentFilePos += segmLenTmp;
                *bytesLeft -= segmLenTmp;
                segmLenTmp  = 0;
            }
            return FLAC_PARSE_OGG_DONE;
        }

        if(s_nBytes > 0){
            // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
            // Было int16_t: при окне 65535 пакет OGG может быть > 32767 → diff уходил в минус
            // → *bytesLeft/filePos ехали в обратную сторону → мгновенный рассинхрон.
            int32_t diff = s_nBytes;
            if(s_flacAudioDataStart == 0){
                s_flacAudioDataStart = s_flacCurrentFilePos;
            }
            ret = FLACDecodeNative(inbuf, &s_nBytes, outbuf);
            diff -= s_nBytes;
            s_flacCurrentFilePos += diff;
            *bytesLeft -= diff;
            // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
            // Эталон FUSION: FLAC-кадр больше OGG-пакета → s_nBytes уходит в минус.
            // Это НОРМА (кадр продолжается в следующих lace/странице), а не рассинхрон:
            // раньше отрицательное значение доезжало до ветки ниже и печатало
            // «Flac decoder asynchron» + сбрасывало пакет. Теперь просто clamp в 0.
            if(s_nBytes < 0) s_nBytes = 0;
            // [v0.8.277] Ошибка внутри OGG-аудиопакета: выбрасываем остаток lace.
            if(ret < 0){
                if(s_nBytes > 0){
                    s_flacCurrentFilePos += s_nBytes;
                    *bytesLeft -= s_nBytes;
                }
                FLACDecoderAbortOggPacket();
            }
            return ret;
        }
        if(s_nBytes < 0){
            FLAC_LOG_ERROR("Flac decoder asynchron");
            FLACDecoderAbortOggPacket();
            s_f_flacParseOgg = true;
            return FLAC_ERR;
        }

        if(s_f_flacParseOgg == true){
            s_f_flacParseOgg = false;
            ret = FLACparseOGG(inbuf, bytesLeft);
            if(ret == FLAC_NONE) return FLAC_PARSE_OGG_DONE; // ok
            else return ret;  // error
        }
        //-------------------------------------------------------
        if(!s_flacSegmTableVec.size()) FLAC_LOG_ERROR("size is 0");
        segmLen = s_flacSegmTableVec.back();
        // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
        s_segmLength = segmLen; // длина текущего lace — для сброса хвоста continued-страницы
        s_flacSegmTableVec.pop_back();
        if(!s_flacSegmTableVec.size()) s_f_flacParseOgg = true;
        //-------------------------------------------------------
        // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
        // Страховка H2: аудиопакет обязан целиком лежать в текущем окне (*bytesLeft), иначе
        // декодер читал бы за его границей. Сначала до 8 раз ждём данных (сегмент возвращаем
        // в таблицу, состояние не меняем), потом честно ДРОПАЕМ пакет через штатный
        // чанковый скип segmLenTmp — так позиция в файле остаётся верной, без каскада ошибок.
        if(s_flacPageNr == 2 && (int32_t)segmLen > *bytesLeft){
            if(pktWaitCnt < 8){
                pktWaitCnt++;
                s_flacSegmTableVec.push_back(segmLen);
                s_f_flacParseOgg = false;
                s_segmLength = 0;
                return FLAC_DECODE_FRAMES_LOOP; // need more data
            }
            pktWaitCnt = 0;
            FLAC_LOG_WARN("Flac: ogg packet %u > window %i, packet dropped", segmLen, *bytesLeft);
            segmLenTmp = segmLen;
            return FLAC_PARSE_OGG_DONE;
        }
        pktWaitCnt = 0;

        if(s_flacRemainBlockPicLen <= 0 && !s_f_flacNewMetadataBlockPicture) {
            if(s_flacBlockPicItem.size() > 0) { // get blockpic data
                // FLAC_LOG_INFO("---------------------------------------------------------------------------");
                // FLAC_LOG_INFO("metadata blockpic found at pos %i, size %i bytes", s_flacBlockPicPos, s_flacBlockPicLen);
                // for(int32_t i = 0; i < s_flacBlockPicItem.size(); i += 2) { FLAC_LOG_INFO("segment %02i, pos %07i, len %05i", i / 2, s_flacBlockPicItem[i], s_flacBlockPicItem[i + 1]); }
                // FLAC_LOG_INFO("---------------------------------------------------------------------------");
                s_f_flacNewMetadataBlockPicture = true;
            }
        }

        switch(s_flacPageNr) {
            case 0:
                ret = parseFlacFirstPacket(inbuf, segmLen);
                if(ret == segmLen) {
                    s_flacPageNr = 1;
                    ret = FLAC_PARSE_OGG_DONE;
                    break;
                }
                if(ret < 0){  // fLaC signature not found
                    break;
                }
                if(ret < segmLen){
                    segmLen -= ret;
                    *bytesLeft -= ret;
                    s_flacCurrentFilePos += ret;
                    inbuf += ret;
                    s_flacPageNr = 1;
                } /* fallthrough */
            case 1:
                if(s_flacRemainBlockPicLen > 0){
                    s_flacRemainBlockPicLen -= segmLen;
                    //FLAC_LOG_INFO("s_flacCurrentFilePos %i, len %i, s_flacRemainBlockPicLen %i", s_flacCurrentFilePos, segmLen, s_flacRemainBlockPicLen);
                    s_flacBlockPicItem.push_back(s_flacCurrentFilePos);
                    s_flacBlockPicItem.push_back(segmLen);
                    if(s_flacRemainBlockPicLen <= 0){s_flacPageNr = 2;}
                    ret = FLAC_PARSE_OGG_DONE;
                    break;
                }
                ret = parseMetaDataBlockHeader(inbuf, segmLen);
                if(s_f_lastMetaDataBlock) s_flacPageNr = 2;
                break;
            case 2:
                // [Opus5] OGG-FLAC Sector Progressive (lstepMOD)
                // Выравнивание с FUSION/Wolle: case 2 НЕ декодирует и НЕ сканирует sync.
                // Только помечаем длину текущего lace → на СЛЕДУЮЩЕМ входе FLACDecode()
                // сработает ветка s_nBytes > 0 → FLACDecodeNative (там H1 continued-page).
                // Старый скан 0xFFF8 (v0.8.277) здесь ловил ложные совпадения → BITRATE ~2.x.
                s_f_bitReaderError = false;       // H2: не глушить следующие исправные кадры
                s_f_bitReaderErrorLogged = false;
                s_nBytes = segmLen;
                return FLAC_PARSE_OGG_DONE;
                break;
        }
        if(segmLen > FLAC_MAX_BLOCKSIZE){
            segmLenTmp = segmLen;
            return FLAC_PARSE_OGG_DONE;
        }
        *bytesLeft -= segmLen;
        s_flacCurrentFilePos += segmLen;
        return ret;
    }
    ret = FLACDecodeNative(inbuf, bytesLeft, outbuf);
    return ret;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t FLACDecodeNative(uint8_t *inbuf, int32_t *bytesLeft, int16_t *outbuf){

    int32_t bl = *bytesLeft;
    static int32_t sbl = 0;

    if(s_flacStatus != OUT_SAMPLES){
        s_rIndex = 0;
        s_flacInptr = inbuf;
    }

    while(s_flacStatus == DECODE_FRAME){// Read a ton of header fields, and ignore most of them
        int32_t ret = flacDecodeFrame (inbuf, bytesLeft);
        if(ret != 0) return ret;
        // [v0.8.278][КРИТИЧНО] Для НЕ-OGG (native raw) FLAC bytesLeft — это весь сетевой буфер
        // (сотни КБ), и проверка "хватит ли ещё 24576 байт про запас" оправдана — данные могут
        // прийти позже, а указатель s_flacInptr на буфер общий и не меняется между вызовами.
        //
        // Но для OGG-FLAC (FLACDecode вызывает нас с bytesLeft = &s_nBytes) это ЦЕЛЫЙ ГОТОВЫЙ
        // OGG-пакет (= один FLAC-кадр), полностью лежащий в памяти прямо сейчас — "ещё бита" НЕ
        // придёт никогда, столько, сколько есть, столько и есть. У плотного/шумного контента
        // (EDM/techno/24-bit, напр. Радио Сектор) сжатый кадр весит близко к 20-25 КБ — вплотную
        // к порогу FLAC_MAX_BLOCKSIZE (24576) и часто НИЖЕ него. Раньше в этом случае декодер
        // прерывал разбор ПОСЛЕ заголовка (return FLAC_DECODE_FRAMES_LOOP), а на следующем вызове
        // FLACDecode() снова передавал сюда s_nBytes>0 с уже ДРУГИМ (следующим по потоку) буфером
        // inbuf — но s_flacStatus остался DECODE_SUBFRAMES/DECODE_FRAME, и в верхней части функции
        // "if(s_flacStatus != OUT_SAMPLES){ s_rIndex=0; s_flacInptr=inbuf; }" переставлял указатель
        // чтения на НАЧАЛО этого нового (чужого) буфера — декодер начинал читать данные СЛЕДУЮЩЕГО
        // кадра как продолжение текущего → мусор → "error in bitreader"/"decoder asynchron" на
        // каждом крупном кадре. Фикс: для OGG-режима этот "запас на будущее" не нужен — пакет уже
        // целиком в буфере, декодируем сразу; от выхода за границы защищает s_f_bitReaderError.
        if(!s_f_oggWrapper && *bytesLeft < FLAC_MAX_BLOCKSIZE) return FLAC_DECODE_FRAMES_LOOP; // need more data
        // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
        // OGG-пакет исчерпан (например, только что выброшен хвостовой сегмент continued-страницы) —
        // выходим из while, иначе цикл крутился бы вечно при bytesLeft == 0 (watchdog/фриз).
        if(s_f_oggWrapper && *bytesLeft <= 0) return FLAC_DECODE_FRAMES_LOOP;
        sbl += bl - *bytesLeft;
    }

    if(s_flacStatus == DECODE_SUBFRAMES){
        // Decode each channel's subframe, then skip footer
        int32_t ret = decodeSubframes(bytesLeft);
        if(ret != 0) return ret;
        s_flacStatus = OUT_SAMPLES;
        sbl += bl - *bytesLeft;
    }

    if(s_flacStatus == OUT_SAMPLES){  // Write the decoded samples
        // blocksize can be much greater than outbuff, so we can't stuff all in once
        // therefore we need often more than one loop (split outputblock into pieces)
        uint32_t blockSize;
        if(s_numOfOutSamples < s_flacOutBuffSize + s_offset) blockSize = s_numOfOutSamples - s_offset;
        else blockSize = s_flacOutBuffSize;

        for (int32_t i = 0; i < blockSize; i++) {
            for (int32_t j = 0; j < FLACMetadataBlock->numChannels; j++) {
                int32_t val = s_samplesBuffer[j][i + s_offset];
                if (FLACMetadataBlock->bitsPerSample == 8) {
                    // 8 bit: unsigned → signed, сдвиг в центр диапазона int16_t
                    val += 128;
                    outbuf[2*i+j] = (int16_t)val;
                } else if (FLACMetadataBlock->bitsPerSample == 24) {
                    // 24 bit → 16 bit: усечение на 8 бит (теряем LSB, сохраняем MSB).
                    // val находится в диапазоне ±8 388 607; >> 8 даёт ±32 767.
                    outbuf[2*i+j] = (int16_t)(val >> 8);
                } else {
                    // 16 bit: прямое присвоение (val уже в диапазоне int16_t)
                    outbuf[2*i+j] = (int16_t)val;
                }
            }
        }

        s_flacValidSamples = blockSize * FLACMetadataBlock->numChannels;
        s_offset += blockSize;
        if(sbl > 0){
            s_flacCompressionRatio = (float)((s_flacValidSamples * 2) * FLACMetadataBlock->numChannels) / sbl; // valid samples are 16 bit
            sbl = 0;
            s_flacBitrate = FLACMetadataBlock->sampleRate * FLACMetadataBlock->bitsPerSample * FLACMetadataBlock->numChannels;
            s_flacBitrate /= s_flacCompressionRatio;
      //      FLAC_LOG_INFO("s_flacBitrate %i, s_flacCompressionRatio %f, FLACMetadataBlock->sampleRate %i ", s_flacBitrate, s_flacCompressionRatio, FLACMetadataBlock->sampleRate);
        }
        if(s_offset != s_numOfOutSamples) return GIVE_NEXT_LOOP;
        if(s_offset > s_numOfOutSamples) { FLAC_LOG_ERROR("offset has a wrong value"); }
        s_offset = 0;
    }

    alignToByte();
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    // Футер CRC-16 кадра: если пакет уже исчерпан и в бит-буфере нет 16 бит, чтение уводило
    // *bytesLeft в минус и взводило s_f_bitReaderError на СЛЕДУЮЩЕМ (нормальном) кадре.
    if(*bytesLeft > 0 || s_flacBitBufferLen >= 16) readUint(16, bytesLeft);

//    s_flacCompressionRatio = (float)m_bytesDecoded / (float)s_numOfOutSamples * FLACMetadataBlock->numChannels * (16/8);
//    FLAC_LOG_INFO("s_flacCompressionRatio % f", s_flacCompressionRatio);
    s_flacStatus = DECODE_FRAME;
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t flacDecodeFrame(uint8_t *inbuf, int32_t *bytesLeft){
    if(FLAC_specialIndexOf(inbuf, "OggS", *bytesLeft) == 0){ // async? => new sync is OggS => reset and decode (not page 0 or 1)
        FLACDecoderReset();
        s_flacPageNr = 2;
        return FLAC_OGG_SYNC_FOUND;
    }
    // [Opus5] OGG-FLAC continued page — Sector Progressive (lstepMOD)
    // ЯДРО ФИКСА (H1). Кадр обязан начинаться с 14-битного sync: 0xFF, затем 111110 + reserved(0) +
    // blocking strategy → допустимы 0xF8 (fixed) и 0xF9 (variable blocksize), маска 0xFE.
    // Если sync нет и это первый кадр после (пере)синхронизации на странице с битом continued —
    // перед нами хвост кадра с предыдущей страницы: выбрасываем сегмент целиком и ждём следующий.
    // Раньше этой проверки не было вообще: заголовок читался из хвоста сжатых данных, отсюда
    // «unimplemented reserved subtype», «reserved channel assignment», «blockSizeOut too big»,
    // «bitreader underflow» → «decoder asynchron» на Sector Progressive.
    if(inbuf[0] != 0xFF || (inbuf[1] & 0xFE) != 0xF8){
        if(s_f_first_flac_frame && s_continued_page){
            s_f_first_flac_frame = false;
            *bytesLeft -= (int32_t)s_segmLength; // хвост предыдущего кадра — пропускаем
            return FLAC_NONE;
        }
        FLAC_LOG_ERROR("Flac sync 0xFFF8 not found");
        return FLAC_ERR;
    }
    readUint(14 + 1, bytesLeft); // synccode + reserved bit
    FLACFrameHeader->blockingStrategy = readUint(1, bytesLeft);
    FLACFrameHeader->blockSizeCode = readUint(4, bytesLeft);
    FLACFrameHeader->sampleRateCode = readUint(4, bytesLeft);
    FLACFrameHeader->chanAsgn = readUint(4, bytesLeft);
    FLACFrameHeader->sampleSizeCode = readUint(3, bytesLeft);
    if(!FLACMetadataBlock->numChannels){
        if(FLACFrameHeader->chanAsgn == 0) FLACMetadataBlock->numChannels = 1;
        if(FLACFrameHeader->chanAsgn == 1) FLACMetadataBlock->numChannels = 2;
        if(FLACFrameHeader->chanAsgn > 7)  FLACMetadataBlock->numChannels = 2;
    }
    if(FLACMetadataBlock->numChannels < 1) {FLAC_LOG_ERROR("Flac unknown channel assignment, ch: %i", FLACMetadataBlock->numChannels); return FLAC_STOP;}
        if(!FLACMetadataBlock->bitsPerSample){
        if(FLACFrameHeader->sampleSizeCode == 1) FLACMetadataBlock->bitsPerSample =  8;
        if(FLACFrameHeader->sampleSizeCode == 2) FLACMetadataBlock->bitsPerSample = 12;
        if(FLACFrameHeader->sampleSizeCode == 4) FLACMetadataBlock->bitsPerSample = 16;
        if(FLACFrameHeader->sampleSizeCode == 5) FLACMetadataBlock->bitsPerSample = 20;
        if(FLACFrameHeader->sampleSizeCode == 6) FLACMetadataBlock->bitsPerSample = 24;
    }
    // Разрешены 8, 16 и 24 bit/sample.
    // 24-bit: при выводе val >> 8 усекается до 16-bit (i2s int16_t буфер).
    // 12 и 20 bit не поддерживаются стандартом FLAC для потокового воспроизведения.
    if (FLACMetadataBlock->bitsPerSample == 12 || FLACMetadataBlock->bitsPerSample == 20) {
        FLAC_LOG_ERROR("Flac, bits per sample must be 8, 16 or 24, is: %i", FLACMetadataBlock->bitsPerSample);
        return FLAC_STOP;
    }
    if (FLACMetadataBlock->bitsPerSample > 24) {
        FLAC_LOG_ERROR("Flac, bits per sample > 24 not supported, is: %i", FLACMetadataBlock->bitsPerSample);
        return FLAC_STOP;
    }
    if (FLACMetadataBlock->bitsPerSample < 8) {
        FLAC_LOG_ERROR("Flac, bits per sample <8, bps: %i", FLACMetadataBlock->bitsPerSample);
        return FLAC_STOP;
    }
    if(!FLACMetadataBlock->sampleRate){
        if(FLACFrameHeader->sampleRateCode == 1)  FLACMetadataBlock->sampleRate =  88200;
        if(FLACFrameHeader->sampleRateCode == 2)  FLACMetadataBlock->sampleRate = 176400;
        if(FLACFrameHeader->sampleRateCode == 3)  FLACMetadataBlock->sampleRate = 192000;
        if(FLACFrameHeader->sampleRateCode == 4)  FLACMetadataBlock->sampleRate =   8000;
        if(FLACFrameHeader->sampleRateCode == 5)  FLACMetadataBlock->sampleRate =  16000;
        if(FLACFrameHeader->sampleRateCode == 6)  FLACMetadataBlock->sampleRate =  22050;
        if(FLACFrameHeader->sampleRateCode == 7)  FLACMetadataBlock->sampleRate =  24000;
        if(FLACFrameHeader->sampleRateCode == 8)  FLACMetadataBlock->sampleRate =  32000;
        if(FLACFrameHeader->sampleRateCode == 9)  FLACMetadataBlock->sampleRate =  44100;
        if(FLACFrameHeader->sampleRateCode == 10) FLACMetadataBlock->sampleRate =  48000;
        if(FLACFrameHeader->sampleRateCode == 11) FLACMetadataBlock->sampleRate =  96000;
    }
    readUint(1, bytesLeft);
    uint32_t temp = (readUint(8, bytesLeft) << 24);
    temp = ~temp;
    uint32_t shift = 0x80000000; // Number of leading zeros
    int8_t count = 0;
    for(int32_t i=0; i<32; i++){
        if((temp & shift) == 0) {count++; shift >>= 1;}
        else break;
    }
    count--;
    for (int32_t i = 0; i < count; i++) readUint(8, bytesLeft);
    s_numOfOutSamples = 0;
    if (FLACFrameHeader->blockSizeCode == 1)
        s_numOfOutSamples = 192;
    else if (2 <= FLACFrameHeader->blockSizeCode && FLACFrameHeader->blockSizeCode <= 5)
        s_numOfOutSamples = 576 << (FLACFrameHeader->blockSizeCode - 2);
    else if (FLACFrameHeader->blockSizeCode == 6)
        s_numOfOutSamples = readUint(8, bytesLeft) + 1;
    else if (FLACFrameHeader->blockSizeCode == 7)
        s_numOfOutSamples = readUint(16, bytesLeft) + 1;
    else if (8 <= FLACFrameHeader->blockSizeCode && FLACFrameHeader->blockSizeCode <= 15)
        s_numOfOutSamples = 256 << (FLACFrameHeader->blockSizeCode - 8);
    else{
        FLAC_LOG_ERROR("Flac, reserved blocksize unsupported, block size code: %i", FLACFrameHeader->blockSizeCode);
        return FLAC_ERR;
    }
    // [v0.8.277] Верхняя граница — размер выделенного s_samplesBuffer (24576), НЕ STREAMINFO.maxblocksize:
    // энкодеры (Sector SPACE и др.) часто шлют кадры чуть больше заявленного max (4096 vs 4608).
    // Старую проверку по FLAC_MAX_OUTBUFFSIZE (8192) убрали — это размер чанка I2S-вывода, не кадра.
    if(s_numOfOutSamples > s_maxBlocksize){
        FLAC_LOG_ERROR("Flac, blockSizeOut too big ,%i samples (hw max %i)", s_numOfOutSamples, s_maxBlocksize);
        return FLAC_ERR;
    }
    if(FLACFrameHeader->sampleRateCode == 12)
        readUint(8, bytesLeft);
    else if (FLACFrameHeader->sampleRateCode == 13 || FLACFrameHeader->sampleRateCode == 14){
        readUint(16, bytesLeft);
    }
    readUint(8, bytesLeft);
    s_flacStatus = DECODE_SUBFRAMES;
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FLACGetOutputSamps(){
    uint32_t vs = s_flacValidSamples;
    s_flacValidSamples=0;
    return vs;
}
//----------------------------------------------------------------------------------------------------------------------
uint64_t FLACGetTotoalSamplesInStream(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->totalSamples;
}
//----------------------------------------------------------------------------------------------------------------------
uint8_t FLACGetBitsPerSample(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->bitsPerSample;
}
//----------------------------------------------------------------------------------------------------------------------
uint8_t FLACGetChannels(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->numChannels;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FLACGetSampRate(){
    if(!FLACMetadataBlock) return 0;
    return FLACMetadataBlock->sampleRate;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FLACGetBitRate(){
    return s_flacBitrate;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FLACGetAudioDataStart(){
    return s_flacAudioDataStart;
}
//----------------------------------------------------------------------------------------------------------------------
uint32_t FLACGetAudioFileDuration() {
    if(FLACGetSampRate()){ // DIV0
        uint32_t afd = FLACGetTotoalSamplesInStream()/ FLACGetSampRate(); // AudioFileDuration
        return afd;
    }
    return 0;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t decodeSubframes(int32_t* bytesLeft){
    if(FLACFrameHeader->chanAsgn <= 7) {
        for (int32_t ch = 0; ch < FLACMetadataBlock->numChannels; ch++)
            decodeSubframe(FLACMetadataBlock->bitsPerSample, ch, bytesLeft);
    }
    else if (8 <= FLACFrameHeader->chanAsgn && FLACFrameHeader->chanAsgn <= 10) {
        decodeSubframe(FLACMetadataBlock->bitsPerSample + (FLACFrameHeader->chanAsgn == 9 ? 1 : 0), 0, bytesLeft);
        decodeSubframe(FLACMetadataBlock->bitsPerSample + (FLACFrameHeader->chanAsgn == 9 ? 0 : 1), 1, bytesLeft);
        if(FLACFrameHeader->chanAsgn == 8) {
            for (int32_t i = 0; i < s_numOfOutSamples; i++)
                s_samplesBuffer[1][i] = (
                        s_samplesBuffer[0][i] -
                        s_samplesBuffer[1][i]);
        }
        else if (FLACFrameHeader->chanAsgn == 9) {
            for (int32_t i = 0; i < s_numOfOutSamples; i++)
                s_samplesBuffer[0][i] += s_samplesBuffer[1][i];
        }
        else if (FLACFrameHeader->chanAsgn == 10) {
            for (int32_t i = 0; i < s_numOfOutSamples; i++) {
                int32_t side =  s_samplesBuffer[1][i];
                int32_t right = s_samplesBuffer[0][i] - (side >> 1);
                s_samplesBuffer[1][i] = right;
                s_samplesBuffer[0][i] = right + side;
            }
        }
        else {
            FLAC_LOG_ERROR("Flac, unknown channel assignment, %i", FLACFrameHeader->chanAsgn);
            return FLAC_ERR;
        }
    }
    else{
        FLAC_LOG_ERROR("Flac reserved channel assignment, %i", FLACFrameHeader->chanAsgn);
        return FLAC_ERR;
    }
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t decodeSubframe(uint8_t sampleDepth, uint8_t ch, int32_t* bytesLeft) {
    int8_t ret = 0;
    readUint(1, bytesLeft);                // Zero bit padding, to prevent sync-fooling string of 1s
    uint8_t type = readUint(6, bytesLeft); // Subframe type: 000000 : SUBFRAME_CONSTANT
                                           //                000001 : SUBFRAME_VERBATIM
                                           //                00001x : reserved
                                           //                0001xx : reserved
                                           //                001xxx : if(xxx <= 4) SUBFRAME_FIXED, xxx=order ; else reserved
                                           //                01xxxx : reserved
                                           //                1xxxxx : SUBFRAME_LPC, xxxxx=order-1

    int32_t shift = readUint(1, bytesLeft);    // Wasted bits-per-sample' flag:
                                           // 0 : no wasted bits-per-sample in source subblock, k=0
                                           // 1 : k wasted bits-per-sample in source subblock, k-1 follows, unary coded; e.g. k=3 => 001 follows, k=7 => 0000001 follows.
    if (shift == 1) {
        // [v0.8.208][КРИТИЧНО] Тот же дефект, что в readRiceSignedInt(): при s_f_bitReaderError
        // readUint() вечно возвращает 0 и unary-цикл «wasted bits» не завершается — зависание всего ядра 1.
        // При ошибке bitreader выходим с FLAC_ERR: кадр битый, декодер выполнит ресинхронизацию.
        while (readUint(1, bytesLeft) == 0) {
            if (s_f_bitReaderError) return FLAC_ERR;
            shift++;
        }
    }
    sampleDepth -= shift;

    if(type == 0){  // Constant coding
        int32_t s= readSignedInt(sampleDepth, bytesLeft);                                    // SUBFRAME_CONSTANT
        for(int32_t i = 0; i < s_numOfOutSamples; i++){
            s_samplesBuffer[ch][i] = s;
        }
    }
    else if (type == 1) {  // Verbatim coding
        for (int32_t i = 0; i < s_numOfOutSamples; i++)
            s_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft);                  // SUBFRAME_VERBATIM
    }
    else if (8 <= type && type <= 12){
        ret = decodeFixedPredictionSubframe(type - 8, sampleDepth, ch, bytesLeft);           // SUBFRAME_FIXED
        if(ret) return ret;
    }
    else if (32 <= type && type <= 63){
        ret = decodeLinearPredictiveCodingSubframe(type - 31, sampleDepth, ch, bytesLeft);   // SUBFRAME_LPC
        if(ret) return ret;
    }
    else{
        FLAC_LOG_ERROR("Flac unimplemented reserved subtype: %i", type);
        return FLAC_ERR;
    }
    if(shift>0){
        for (int32_t i = 0; i < s_numOfOutSamples; i++){
            s_samplesBuffer[ch][i] <<= shift;
        }
    }
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------------------------------------
int8_t decodeFixedPredictionSubframe(uint8_t predOrder, uint8_t sampleDepth, uint8_t ch, int32_t* bytesLeft) {     // SUBFRAME_FIXED

    uint8_t ret = 0;
    for(uint8_t i = 0; i < predOrder; i++)
        s_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft); // Unencoded warm-up samples (n = frame's bits-per-sample * predictor order).
    ret = decodeResiduals(predOrder, ch, bytesLeft);
    if(ret) return ret;
    coefs.clear(); coefs.shrink_to_fit();
    if(predOrder == 0) coefs.resize(0);
    if(predOrder == 1) coefs.push_back(1);  // FIXED_PREDICTION_COEFFICIENTS
    if(predOrder == 2){coefs.push_back(2); coefs.push_back(-1);}
    if(predOrder == 3){coefs.push_back(3); coefs.push_back(-3); coefs.push_back(1);}
    if(predOrder == 4){coefs.push_back(4); coefs.push_back(-6); coefs.push_back(4); coefs.push_back(-1);}
    if(predOrder > 4) {FLAC_LOG_ERROR("Flac preorder too big: %i", predOrder); return FLAC_ERR;} // Error: preorder > 4"
    restoreLinearPrediction(ch, 0);
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t decodeLinearPredictiveCodingSubframe(int32_t lpcOrder, int32_t sampleDepth, uint8_t ch, int32_t* bytesLeft){

    int8_t ret = 0;
    for (int32_t i = 0; i < lpcOrder; i++){
        s_samplesBuffer[ch][i] = readSignedInt(sampleDepth, bytesLeft); // Unencoded warm-up samples (n = frame's bits-per-sample * lpc order).
    }
    int32_t precision = readUint(4, bytesLeft) + 1;                         // (Quantized linear predictor coefficients' precision in bits)-1 (1111 = invalid).
    int32_t shift = readSignedInt(5, bytesLeft);                            // Quantized linear predictor coefficient shift needed in bits (NOTE: this number is signed two's-complement).
    coefs.clear(); coefs.shrink_to_fit();
    for (uint8_t i = 0; i < lpcOrder; i++){
        coefs.push_back(readSignedInt(precision, bytesLeft));           // Unencoded predictor coefficients (n = qlp coeff precision * lpc order) (NOTE: the coefficients are signed two's-complement).
    }
    ret = decodeResiduals(lpcOrder, ch, bytesLeft);
    if(ret) return ret;
    restoreLinearPrediction(ch, shift);
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
int8_t decodeResiduals(uint8_t warmup, uint8_t ch, int32_t* bytesLeft) {

    int32_t method = readUint(2, bytesLeft);                          // Residual coding method:
                                                                  // 00 : partitioned Rice coding with 4-bit Rice parameter; RESIDUAL_CODING_METHOD_PARTITIONED_RICE follows
                                                                  // 01 : partitioned Rice coding with 5-bit Rice parameter; RESIDUAL_CODING_METHOD_PARTITIONED_RICE2 follows
                                                                  // 10-11 : reserved
    if (method >= 2) {FLAC_LOG_ERROR("Flac reserved residual coding, method: %i", method); return FLAC_ERR;}
    uint8_t paramBits = method == 0 ? 4 : 5;                      // RESIDUAL_CODING_METHOD_PARTITIONED_RICE || RESIDUAL_CODING_METHOD_PARTITIONED_RICE2
    int32_t escapeParam = ( method == 0 ? 0xF : 0x1F);
    int32_t partitionOrder = readUint(4, bytesLeft);                  // Partition order
    int32_t numPartitions = 1 << partitionOrder;                      // There will be 2^order partitions.

    if (s_numOfOutSamples % numPartitions != 0){
        FLAC_LOG_ERROR("Flac, wrong rice partition number");
        return FLAC_ERR;                  //Error: Block size not divisible by number of Rice partitions
    }
    int32_t partitionSize = s_numOfOutSamples / numPartitions;

    for (int32_t i = 0; i < numPartitions; i++) {
        int32_t start = i * partitionSize + (i == 0 ? warmup : 0);
        int32_t end = (i + 1) * partitionSize;

        int32_t param = readUint(paramBits, bytesLeft);
        if (param < escapeParam) {
            for (int32_t j = start; j < end; j++){
                if(s_f_bitReaderError) break;
                s_samplesBuffer[ch][j] = readRiceSignedInt(param, bytesLeft);
            }
        }
        else {
            int32_t numBits = readUint(5, bytesLeft);                 // Escape code, meaning the partition is in unencoded binary form using n bits per sample; n follows as a 5-bit number.
            for (int32_t j = start; j < end; j++){
                if(s_f_bitReaderError) break;
                s_samplesBuffer[ch][j] = readSignedInt(numBits, bytesLeft);
            }
        }
    }
    if(s_f_bitReaderError) {FLAC_LOG_ERROR("Flac bitreader underflow"); return FLAC_ERR;}
    return FLAC_NONE;
}
//----------------------------------------------------------------------------------------------------------------------
void restoreLinearPrediction(uint8_t ch, uint8_t shift) {

    for (int32_t i = coefs.size(); i < s_numOfOutSamples; i++) {
        int32_t sum = 0;
        for (int32_t j = 0; j < coefs.size(); j++){
            sum += s_samplesBuffer[ch][i - 1 - j] * coefs[j];
        }
        s_samplesBuffer[ch][i] += (sum >> shift);
    }
}
//----------------------------------------------------------------------------------------------------------------------
int32_t FLAC_specialIndexOf(uint8_t* base, const char* str, int32_t baselen, bool exact){
    int32_t result = 0;  // seek for str in buffer or in header up to baselen, not nullterninated
    if (strlen(str) > baselen) return -1; // if exact == true seekstr in buffer must have "\0" at the end
    for (int32_t i = 0; i < baselen - strlen(str); i++){
        result = i;
        for (int32_t j = 0; j < strlen(str) + exact; j++){
            if (*(base + i + j) != *(str + j)){
                result = -1;
                break;
            }
        }
        if (result >= 0) break;
    }
    return result;
}
//----------------------------------------------------------------------------------------------------------------------
