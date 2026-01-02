#ifndef STORAGE_HPP
#define STORAGE_HPP

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <shared_mutex>

struct Element {
    std::string name;
};

struct RecipeRecord {
    uint32_t first;  
    uint32_t second; 
    uint32_t result; 
};

class GameData {
public:
    static const uint32_t NOTHING_ID = 0;

    ~GameData() {
        if (elements_out.is_open()) elements_out.close();
        if (recipes_out.is_open()) recipes_out.close();
    }

    std::pair<unsigned char, size_t> load() {
        std::pair<unsigned char, size_t> res = { 0, 0 };
        std::call_once(loaded, [&](){
            if (!loadElements()) {
                res = { 2, 0 };
                return;
            }
            if (!loadRecipes(elements.size())) {
                res = { 2, 1 };
                return;
            }

            elements_out.open("elements.bin", std::ios::binary | std::ios::app);
            recipes_out.open("recipes.bin", std::ios::binary | std::ios::app);

            if (elements.size() > (std::numeric_limits<uint32_t>::max() / 2)) {
                res = { 1, elements.size() };
            } else {
                res = { 0, elements.size() };
            }
        });
        return res;
    }

    std::string getName(const uint32_t& id) const {
        std::shared_lock lock(elements_mutex);

        static const std::string empty = "";
        return id < elements.size() ? elements[id].name : empty;
    }

    std::vector<std::string> getNames(std::vector<uint32_t> ids) const {
        std::shared_lock lock(elements_mutex);

        std::vector<std::string> result;
        result.reserve(ids.size());
        for (const uint32_t& id : ids)
            if (id < elements.size()) result.push_back(elements[id].name);
            else result.push_back("");
        return result;
    }

    std::pair<uint32_t, bool> addElement(const std::string& name, const std::string& emoji, bool isGlobalNew) {
        std::unique_lock lock(elements_mutex);

        if (auto it = name_to_id.find(name); it != name_to_id.end()) return {it->second, false};

        uint32_t id = (uint32_t)elements.size();
        elements.push_back({name});
        name_to_id[elements.back().name] = id;
        
        appendElementToDisk(name, emoji, isGlobalNew);
        
        return {id, true};
    }
    
    size_t getElementCount() const {
        std::shared_lock lock(elements_mutex);

        return elements.size();
    }
    size_t getRecipeCount() const {
        std::shared_lock lock(recipes_mutex);

        return known_recipes.size();
    }

    uint32_t getResult(uint32_t idA, uint32_t idB) const {
        std::shared_lock lock(recipes_mutex);

        if (idA > idB) std::swap(idA, idB);

        auto it = std::lower_bound(known_recipes.begin(), known_recipes.end(), std::make_pair(idA, idB), 
            [](const RecipeRecord& r, const std::pair<uint32_t, uint32_t>& val) {
                if (r.second != val.second) return r.second < val.second;
                return r.first < val.first;
            });

        if (it != known_recipes.end() && it->first == idA && it->second == idB) return it->result;
        return UINT32_MAX;
    }

    void addRecipe(uint32_t idA, uint32_t idB, uint32_t resultId) {
        std::unique_lock lock(recipes_mutex);

        if (idA > idB) std::swap(idA, idB);

        auto it = std::lower_bound(known_recipes.begin(), known_recipes.end(), std::make_pair(idA, idB), 
            [](const RecipeRecord& r, const std::pair<uint32_t, uint32_t>& val) {
                if (r.second != val.second) return r.second < val.second;
                return r.first < val.first;
            });

        if (it != known_recipes.end() && it->first == idA && it->second == idB) return;

        known_recipes.insert(it, {idA, idB, resultId});
        appendRecipeToDisk(idA, idB, resultId);
    }

    void initDefaults() {
        if (elements.empty()) {
            addElement("Nothing", "", false); 
            addElement("Water", "💧", false);
            addElement("Fire", "🔥", false);
            addElement("Wind", "🌬️", false);
            addElement("Earth", "🌍", false);
        }
    }

private:
    std::once_flag loaded;

    mutable std::shared_mutex elements_mutex;
        std::deque<Element> elements;
        std::unordered_map<std::string_view, uint32_t> name_to_id;

    mutable std::shared_mutex recipes_mutex;
        std::vector<RecipeRecord> known_recipes;

    // Cache the file streams to improve performance
    std::ofstream elements_out;
    std::ofstream recipes_out;

    bool loadElements() {
        std::ifstream f("elements.bin", std::ios::binary);
        if (!f.is_open()) return true; // File doesn't exist

        elements.clear();
        name_to_id.clear();

        while (f.peek() != EOF) {
            uint16_t nameLen = 0;
            uint16_t emojiLen = 0;
            bool isNew = false;
            
            if (!f.read((char*)&nameLen, sizeof(nameLen))) return false; // Partial data
            if (nameLen > 4096) return false; // Name too long

            std::string name(nameLen, '\0');
            if (!f.read(&name[0], nameLen)) return false; // Missing data (name)

            if (!f.read((char*)&emojiLen, sizeof(emojiLen))) return false; // Missing data (emoji)
            if (emojiLen > 4096) return false; // Emoji too long

            f.ignore(emojiLen); 
            if (f.gcount() != emojiLen) return false; // Unexpected EOF (emoji)

            if (!f.read((char*)&isNew, sizeof(isNew))) return false; // Unexpected EOF (New Flag)

            uint32_t id = (uint32_t)elements.size();
            elements.push_back({name});
            name_to_id[elements.back().name] = id;
        }
        
        return true;
    }

    bool loadRecipes(size_t maxElementId) {
        std::ifstream f("recipes.bin", std::ios::binary);
        if (!f.is_open()) return true; 

        f.seekg(0, std::ios::end);
        size_t fileSize = f.tellg();
        f.seekg(0, std::ios::beg);

        if (fileSize == 0) return true;
        if (fileSize % sizeof(RecipeRecord) != 0) return false; 

        size_t count = fileSize / sizeof(RecipeRecord);
        known_recipes.resize(count);
        
        if (!f.read((char*)known_recipes.data(), fileSize)) return false; 

        // Canonicalize (ensure first < second)
        for (auto& r : known_recipes) {
            if (r.first >= maxElementId || r.second >= maxElementId || r.result >= maxElementId) return false;
            if (r.first > r.second) std::swap(r.first, r.second);
        }

        // Sort by Second ID (Primary), then First ID (Secondary)
        std::sort(known_recipes.begin(), known_recipes.end(), 
            [](const RecipeRecord& a, const RecipeRecord& b) {
                if (a.second != b.second) return a.second < b.second;
                return a.first < b.first;
            });

        // Deduplicate
        auto last = std::unique(known_recipes.begin(), known_recipes.end(),
            [](const RecipeRecord& a, const RecipeRecord& b) {
                return a.first == b.first && a.second == b.second;
            });
        known_recipes.erase(last, known_recipes.end());

        return true;
    }

    void appendElementToDisk(const std::string& name, const std::string& emoji, bool isNew) {
        if (!elements_out.is_open()) return; // Safety check

        uint16_t nameLen = (uint16_t)name.size();
        uint16_t emojiLen = (uint16_t)emoji.size();
        
        elements_out.write((char*)&nameLen, sizeof(nameLen));
        elements_out.write(name.data(), nameLen);
        elements_out.write((char*)&emojiLen, sizeof(emojiLen));
        elements_out.write(emoji.data(), emojiLen);
        elements_out.write((char*)&isNew, sizeof(isNew));
        
        // Flush to ensure data is safe in case of crash
        elements_out.flush();
    }

    void appendRecipeToDisk(uint32_t a, uint32_t b, uint32_t res) {
        if (!recipes_out.is_open()) return;

        if (a > b) std::swap(a, b);
        RecipeRecord r = {a, b, res};
        recipes_out.write((char*)&r, sizeof(r));
        recipes_out.flush(); 
    }
};

#endif