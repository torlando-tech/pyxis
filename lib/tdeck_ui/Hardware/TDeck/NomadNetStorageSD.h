// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#ifndef HARDWARE_TDECK_NOMADNET_STORAGE_SD_H
#define HARDWARE_TDECK_NOMADNET_STORAGE_SD_H
#include "UI/LXMF/NomadNetStorage.h"
#ifdef ARDUINO
#include "SDAccess.h"
#include <FS.h>
#include <dirent.h>
namespace Hardware { namespace TDeck {
class NomadNetStorageSD final : public UI::LXMF::NomadNet::NomadNetStorage {
public:
    NomadNetStorageSD(); ~NomadNetStorageSD() override;
    bool isAvailable() const override;
    UI::LXMF::NomadNet::StorageResult beginRead(const char*,std::uint32_t&) override;
    UI::LXMF::NomadNet::StorageResult readChunk(std::uint8_t*,std::size_t,std::size_t&) override;
    UI::LXMF::NomadNet::StorageResult endRead() override;
    UI::LXMF::NomadNet::StorageResult beginWrite(const char*) override;
    UI::LXMF::NomadNet::StorageResult writeChunk(const std::uint8_t*,std::size_t,std::size_t&) override;
    UI::LXMF::NomadNet::StorageResult commitWrite() override;
    UI::LXMF::NomadNet::StorageResult abortWrite() override;
    UI::LXMF::NomadNet::StorageResult remove(const char*) override;
    UI::LXMF::NomadNet::StorageResult rename(const char*,const char*) override;
    UI::LXMF::NomadNet::StorageResult stat(const char*,std::uint32_t&) override;
    UI::LXMF::NomadNet::StorageResult beginList(const char*) override;
    UI::LXMF::NomadNet::StorageResult nextList(char*,std::size_t,bool&) override;
    UI::LXMF::NomadNet::StorageResult endList() override;
private:
    fs::File read_;DIR* list_dir_=nullptr;char list_base_[128]={};int write_fd_=-1;bool writing_=false,abort_pending_=false,healthy_=true;
    static bool cardPresentLocked();static bool mountedPath(const char*,char*,std::size_t);
    static bool parentsLocked(const char*);
    UI::LXMF::NomadNet::StorageResult serviceAbortLocked();
    void poisonWrite();
};
}}
#endif
#endif
