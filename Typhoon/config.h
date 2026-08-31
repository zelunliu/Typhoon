/**
 * @file  config.h
 * @brief performance autotuning configuration manager
 *
 * @author Zelun Liu (Texas A&M University)
 * @author Arif Arman (Texas A&M University)
 * @author Dmitri Loguinov (Texas A&M University)
 *
 * Copyright (C) 2025 - 2026 Zelun Liu, Arif Arman, and Dmitri Loguinov.
 * All rights reserved.
 *
 * The 3-clause BSD License is applied to this software, see
 * license.txt
 */

#pragma once

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <algorithm>
#include <sstream>
#include <iomanip>

// ============================================================================
// CPU-Aware Configuration Manager
// ============================================================================
// Usage Example:
// 
//     Config conf;
//     conf.load("config.ini", 64); // Attempt to load 64-bit tuned parameters
// 
//     if (conf.empty()) { // not available for this machine and this keytype
//         // ... Benchmark ...
//         conf["WC_LINE"] = best_wc;
//         conf["SIMD"] = best_simd;
//         conf["PrefetchT2"] = best_pref;
//         
//         conf.save("config.ini", 64); // Save for future runs
//     }
// 
//     // Use the tuned parameters
//     int wc_line = conf["WC_LINE"];
// ============================================================================

inline std::string trim(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first + 1));
}

inline std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// Convert an integer bit-width parameter to the exact string tag expected in config.ini
inline std::string formatBitstring(int bits) {
    return "{" + std::to_string(bits) + "-bit}";
}

inline std::string getCPUBrandString() {
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 0x80000000);
    unsigned int nExIds = cpuInfo[0];

    char brand[49] = { 0 };
    if (nExIds >= 0x80000004) {
        __cpuid((int*)(brand), 0x80000002);
        __cpuid((int*)(brand + 16), 0x80000003);
        __cpuid((int*)(brand + 32), 0x80000004);
    }
    else {
        return "Unknown CPU Architecture";
    }
    return trim(std::string(brand));
}

struct CPUSignature {
    std::string family;
    std::string model;
    std::string stepping;

    bool operator<(const CPUSignature& other) const {
        if (family != other.family) return family < other.family;
        if (model != other.model) return model < other.model;
        return stepping < other.stepping;
    }
};

class Config {
private:
    std::map<std::string, int> data;

    CPUSignature getCurrentSignature() const {
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        unsigned int eax = cpuInfo[0];

        unsigned int stepVal = eax & 0xF;
        unsigned int baseModel = (eax >> 4) & 0xF;
        unsigned int baseFamily = (eax >> 8) & 0xF;
        unsigned int extModel = (eax >> 16) & 0xF;
        unsigned int extFamily = (eax >> 20) & 0xFF;

        unsigned int actualFamily = (extFamily << 4) | baseFamily;
        unsigned int actualModel = (extModel << 4) | baseModel;

        std::stringstream f_ss, m_ss, s_ss;
        f_ss << std::hex << actualFamily;
        m_ss << std::hex << actualModel;
        s_ss << std::hex << stepVal;

        return { to_upper(f_ss.str()), to_upper(m_ss.str()), to_upper(s_ss.str()) };
    }

public:
    // Load tracking a custom user-provided bit environment target (e.g., 32 or 64)
    void load(const std::string& filename, int bits) {
        CPUSignature target = getCurrentSignature();
        std::string targetBitContext = formatBitstring(bits);
        std::ifstream file(filename);
        std::string line;

        data.clear();

        bool insideMatchedHardware = false;
        bool insideMatchedBitContext = false;
        std::string currentSectionLabel = "";
        std::string targetSectionLabel = "";
        CPUSignature currentSectionSig = { "", "", "" };

        while (std::getline(file, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;

            if (t[0] == '[' && t.back() == ']') {
                insideMatchedHardware = false;
                insideMatchedBitContext = false;
                currentSectionLabel = t.substr(1, t.size() - 2);
                currentSectionSig = { "", "", "" };
                continue;
            }

            if (t == "{32-bit}" || t == "{64-bit}") {
                insideMatchedBitContext = (t == targetBitContext && insideMatchedHardware);
                continue;
            }

            size_t sep = t.find('=');
            if (sep != std::string::npos) {
                std::string key = trim(t.substr(0, sep));
                std::string valStr = to_upper(trim(t.substr(sep + 1)));

                if (key == "family")        currentSectionSig.family = valStr;
                else if (key == "model")    currentSectionSig.model = valStr;
                else if (key == "stepping") currentSectionSig.stepping = valStr;

                if (currentSectionSig.family == target.family &&
                    currentSectionSig.model == target.model &&
                    currentSectionSig.stepping == target.stepping) {
                    insideMatchedHardware = true;
                    targetSectionLabel = currentSectionLabel;
                }

                if (insideMatchedHardware && insideMatchedBitContext &&
                    (key == "WC_LINE" || key == "SIMD" || key == "PrefetchT2")) {
                    try { data[key] = std::stoi(valStr); }
                    catch (...) {}
                }
            }
        }

        std::cout << "Running on '" << getCPUBrandString()
            << "' (family = " << target.family
            << ", model = " << target.model
            << ", stepping = " << target.stepping << ")\n";

        if (!empty()) {
            std::cout << "Found a match to '" << targetSectionLabel << " " << targetBitContext
                << "' in config.ini" << "\n";
            std::cout << "* Selected: WC_LINE = " << data["WC_LINE"]
                << ", SIMD = " << data["SIMD"]
                << ", PrefetchT2 = " << data["PrefetchT2"] << "\n\n";
        }
        else {
            std::cout << "No matching '" << getCPUBrandString() << " " << targetBitContext <<
                "' footprint found in config.ini" << "\n";
            std::cout << "Initiating Profiling...\n\n";
        }
    }

    // Save mapping custom target bit configurations down into structural keys
    void save(const std::string& filename, int bits) {
        CPUSignature mySig = getCurrentSignature();
        std::string myBrand = getCPUBrandString();
        std::string activeBits = formatBitstring(bits);

        struct BitConfig {
            std::map<std::string, int> params;
            bool exists = false;
        };
        struct SectionData {
            std::string brandName;
            std::map<std::string, BitConfig> bitModes;
        };
        std::map<CPUSignature, SectionData> database;

        std::ifstream inFile(filename);
        if (inFile.is_open()) {
            std::string line;
            CPUSignature loopSig = { "", "", "" };
            std::string loopBrand = "";
            std::string loopCurrentBitMode = "";

            while (std::getline(inFile, line)) {
                std::string t = trim(line);
                if (t.empty() || t[0] == '#') continue;

                if (t[0] == '[' && t.back() == ']') {
                    loopSig = { "", "", "" };
                    loopBrand = t.substr(1, t.size() - 2);
                    loopCurrentBitMode = "";
                    continue;
                }

                if (t == "{32-bit}" || t == "{64-bit}") {
                    loopCurrentBitMode = t;
                    if (!loopSig.family.empty() && !loopSig.model.empty() && !loopSig.stepping.empty()) {
                        database[loopSig].brandName = loopBrand;
                        database[loopSig].bitModes[loopCurrentBitMode].exists = true;
                    }
                    continue;
                }

                size_t sep = line.find('=');
                if (sep != std::string::npos) {
                    std::string k = trim(line.substr(0, sep));
                    std::string v = to_upper(trim(line.substr(sep + 1)));

                    if (k == "family")      loopSig.family = v;
                    else if (k == "model")  loopSig.model = v;
                    else if (k == "stepping") loopSig.stepping = v;
                    else if (!loopCurrentBitMode.empty()) {
                        try { database[loopSig].bitModes[loopCurrentBitMode].params[k] = std::stoi(v); }
                        catch (...) {}
                    }
                }
            }
            inFile.close();
        }

        database[mySig].brandName = myBrand;
        database[mySig].bitModes[activeBits].exists = true;
        database[mySig].bitModes[activeBits].params["WC_LINE"] = data["WC_LINE"];
        database[mySig].bitModes[activeBits].params["SIMD"] = data["SIMD"];
        database[mySig].bitModes[activeBits].params["PrefetchT2"] = data["PrefetchT2"];

        std::ofstream outFile(filename);
        outFile << "# Typhoon Profiler Configuration File\n\n";
        for (auto const& [sig, sec] : database) {
            outFile << "[" << sec.brandName << "]\n";
            outFile << "    family=" << sig.family << "\n";
            outFile << "    model=" << sig.model << "\n";
            outFile << "    stepping=" << sig.stepping << "\n";

            for (std::string modeStr : {"{32-bit}", "{64-bit}"}) {
                if (sec.bitModes.count(modeStr) && sec.bitModes.at(modeStr).exists) {
                    auto const& modeData = sec.bitModes.at(modeStr);
                    outFile << "    " << modeStr << "\n";
                    outFile << "        WC_LINE=" << (modeData.params.count("WC_LINE") ? modeData.params.at("WC_LINE") : 0) << "\n";
                    outFile << "        SIMD=" << (modeData.params.count("SIMD") ? modeData.params.at("SIMD") : 0) << "\n";
                    outFile << "        PrefetchT2=" << (modeData.params.count("PrefetchT2") ? modeData.params.at("PrefetchT2") : 0) << "\n";
                }
            }
            outFile << "\n";
        }
    }

    int& operator[](const std::string& key) { return data[key]; }
    int operator[](const std::string& key) const {
        auto it = data.find(key);
        return (it != data.end()) ? it->second : 0;
    }

    bool empty() const {
        return (data.find("WC_LINE") == data.end() ||
            data.find("SIMD") == data.end() ||
            data.find("PrefetchT2") == data.end());
    }
};