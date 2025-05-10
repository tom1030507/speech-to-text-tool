#include <curl/curl.h>
#include <string>
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <iomanip>

using json = nlohmann::json;

// 回調函數：收集回應資料
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* response = static_cast<std::string*>(userdata);
    response->append(ptr, size * nmemb);
    return size * nmemb;
}

// 回調函數：收集標頭資料
size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* headers = static_cast<std::string*>(userdata);
    headers->append(ptr, size * nmemb);
    return size * nmemb;
}

// 獲取檔案大小
size_t get_file_size(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("無法開啟檔案: " + file_path);
    }
    return file.tellg();
}

// 獲取 MIME 類型
std::string get_mime_type(const std::string& file_path) {
    auto ends_with = [](const std::string& str, const std::string& suffix) {
        return str.size() >= suffix.size() &&
            str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

    if (ends_with(file_path, ".mp3")) {
        return "audio/mpeg";
    }
    else if (ends_with(file_path, ".wav")) {
        return "audio/wav";
    }
    else if (ends_with(file_path, ".m4a")) {
        return "audio/m4a";
    }
    return "application/octet-stream";
}

// 將字串轉為十六進位表示以除錯
std::string to_hex(const std::string& input) {
    std::stringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (unsigned char c : input) {
        hex_stream << std::setw(2) << static_cast<int>(c) << " ";
    }
    return hex_stream.str();
}

// 初始化可續傳上傳
std::string initiate_upload(const std::string& api_key, const std::string& file_path, const std::string& display_name) {
    std::string url = "https://generativelanguage.googleapis.com/upload/v1beta/files?key=" + api_key;
    size_t file_size = get_file_size(file_path);
    std::string mime_type = get_mime_type(file_path);

    json payload = { {"file", {{"display_name", display_name}}} };
    std::string json_payload = payload.dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("無法初始化 curl");
    }

    std::string response;
    std::string headers;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());

    struct curl_slist* header_list = NULL;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    header_list = curl_slist_append(header_list, "X-Goog-Upload-Protocol: resumable");
    header_list = curl_slist_append(header_list, "X-Goog-Upload-Command: start");
    header_list = curl_slist_append(header_list, ("X-Goog-Upload-Header-Content-Length: " + std::to_string(file_size)).c_str());
    header_list = curl_slist_append(header_list, ("X-Goog-Upload-Header-Content-Type: " + mime_type).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);

    long http_code = 0;
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_perform() 失敗: " + std::string(curl_easy_strerror(res)));
    }

    if (http_code != 200) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP " + std::to_string(http_code) + " 回應: " + response);
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    std::string upload_url;
    std::string prefix = "x-goog-upload-url: ";
    std::string headers_lower = headers;
    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);
    size_t pos = headers_lower.find(prefix);
    if (pos != std::string::npos) {
        size_t start = pos + prefix.length();
        size_t end = headers.find("\r\n", start);
        if (end == std::string::npos) end = headers.length();
        upload_url = headers.substr(start, end - start);
    }
    else {
        throw std::runtime_error("無法找到 x-goog-upload-url 標頭。標頭內容: " + headers);
    }

    return upload_url;
}

// 上傳檔案二進位資料
std::string upload_file(const std::string& upload_url, const std::string& file_path) {
    size_t file_size = get_file_size(file_path);

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("無法初始化 curl");
    }

    std::string response;

    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        curl_easy_cleanup(curl);
        throw std::runtime_error("無法開啟檔案: " + file_path);
    }

    std::vector<char> file_data(file_size);
    file.read(file_data.data(), file_size);
    file.close();

    curl_easy_setopt(curl, CURLOPT_URL, upload_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, file_data.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, file_size);

    struct curl_slist* header_list = NULL;
    header_list = curl_slist_append(header_list, ("Content-Length: " + std::to_string(file_size)).c_str());
    header_list = curl_slist_append(header_list, "X-Goog-Upload-Offset: 0");
    header_list = curl_slist_append(header_list, "X-Goog-Upload-Command: upload, finalize");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_perform() 失敗: " + std::string(curl_easy_strerror(res)));
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return response;
}

// 使用檔案 URI 生成內容
std::string generate_content(const std::string& api_key, const std::string& file_uri, const std::string& mime_type) {
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + api_key;

    // 修正後的 JSON 結構：將 text 和 file_data 分離到 parts 陣列
    json payload = {
        {"contents", {{
            {"parts", {
                {{"text", "請將此會議音檔完整轉錄為繁體中文逐字稿，請務必遵守以下格式與規則，否則視為不合格：\n\n1. 【最重要】發言人識別：\n　　- 每位發言人必須明確標示。\n　　- 若音檔中提及姓名或職稱（如「主席」、「陳教授」），請直接使用。\n　　- 若無法得知姓名，請依發言順序命名為「發言人1」、「發言人2」等。\n2. 語言：使用繁體中文。\n3. 時間標記：\n　　- 每位發言人**首次發言**時，在該行句首標註時間（格式：[HH:MM:SS]）。\n4. 內容完整性：\n　　- 完整保留所有口語內容（口頭禪、重複、自我修正、填充詞等）。\n　　- 不可進行任何內容潤飾、省略或重寫。\n\n輸出格式範例如下：\n[00:00:01] 主席: 大家早。\n[00:00:03] 發言人1: 呃，我想先確認一下…"}},
                {{"file_data", {{"mime_type", mime_type}, {"file_uri", file_uri}}}}
            }}
        }}}
    };
    std::string json_payload = payload.dump();

    // 輸出 JSON 請求以除錯
    //std::cout << "Sent JSON Request: " << json_payload << std::endl;

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());

    struct curl_slist* header_list = NULL;
    header_list = curl_slist_append(header_list, "Content-Type: application/json; charset=utf-8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    // 啟用詳細輸出以便除錯
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    }

    // 檢查 HTTP 狀態碼
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        // 記錄原始回應以除錯
        //std::cout << "Raw Response (Hex): " << to_hex(response) << std::endl;
        throw std::runtime_error("HTTP " + std::to_string(http_code) + " response: " + response);
    }

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    return response;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    curl_global_init(CURL_GLOBAL_ALL);

    try {
        // 配置
        std::string api_key = "YOUR_GEMINI_API_KEY";
        std::string audio_file_path = "YOUR_AUDIO_FILE_PATH";
        std::string display_name = "AUDIO";

        // 步驟 1：初始化上傳
        std::string upload_url = initiate_upload(api_key, audio_file_path, display_name);
        std::cout << "Upload URL: " << upload_url << std::endl;

        // 步驟 2：上傳檔案
        std::string file_info = upload_file(upload_url, audio_file_path);
        json file_json = json::parse(file_info);
        std::string file_uri = file_json["file"]["uri"].get<std::string>();
        std::cout << "File URI: " << file_uri << std::endl;

        // 步驟 3：生成內容
        std::string mime_type = get_mime_type(audio_file_path);
        std::string response = generate_content(api_key, file_uri, mime_type);

        // 將回應寫入檔案
        std::ofstream out_file("output.txt", std::ios::out | std::ios::binary);
        if (out_file) {
            out_file << response << std::endl;
            out_file.close();
            std::cout << "Response written to output.txt" << std::endl;
        }

        // 解析並顯示回應
        json response_json = json::parse(response);
        //std::cout << "Full Response: " << response_json.dump(2) << std::endl;

        for (const auto& candidate : response_json["candidates"]) {
            for (const auto& part : candidate["content"]["parts"]) {
                if (part.contains("text")) {
                    std::cout << "Text:\n" << part["text"].get<std::string>() << std::endl;
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    curl_global_cleanup();
    return 0;
}