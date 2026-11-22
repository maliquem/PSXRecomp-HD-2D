// SYSTEM.CNF beyond 16 MiB wins over unrelated early cdrom: strings.
#include "disc_identity.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs=std::filesystem;
static void both32(uint8_t* p,uint32_t value) {
    for(unsigned i=0;i<4;i++) {p[i]=(uint8_t)(value>>(8*i));p[7-i]=p[i];}
}
static void fixture(const fs::path& path,bool raw,
                    const std::string& boot="SLPS_009.98",bool with_decoy=true) {
    const unsigned unit=raw?2352:2048,offset=raw?24:0;
    std::ofstream file(path,std::ios::binary);
    file.seekp(10000ull*unit-1);file.put(0);
    if(raw) {
        uint8_t sync[16]={};std::memset(sync+1,255,10);sync[15]=2;
        file.seekp(0);file.write((char*)sync,sizeof sync);
    }
    auto write=[&](unsigned lba,const std::vector<uint8_t>& data) {
        file.seekp((uint64_t)lba*unit+offset);
        file.write((const char*)data.data(),data.size());
    };
    std::vector<uint8_t> pvd(2048);
    pvd[0]=1;std::memcpy(pvd.data()+1,"CD001",5);pvd[6]=1;
    auto* root=pvd.data()+156;root[0]=34;both32(root+2,20);both32(root+10,2048);
    root[25]=2;root[28]=root[31]=root[32]=1;
    write(16,pvd);
    const std::string cnf="BOOT = cdrom:"+boot+";1\r\n";
    std::vector<uint8_t> directory(2048);
    directory[0]=46;both32(directory.data()+2,9000);both32(directory.data()+10,(uint32_t)cnf.size());
    directory[28]=directory[31]=1;directory[32]=12;
    std::memcpy(directory.data()+33,"SYSTEM.CNF;1",12);write(20,directory);
    write(9000,std::vector<uint8_t>(cnf.begin(),cnf.end()));
    const std::string decoy="BOOT = cdrom:SLUS_999.99;1\r\n";
    if(with_decoy) write(40,std::vector<uint8_t>(decoy.begin(),decoy.end()));
}
int main() {
    auto root=fs::temp_directory_path()/ ("psx-disc-identity-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    for(bool raw:{false,true}) {
        auto image=root/(raw?"disc.bin":"disc.iso");fixture(image,raw);
        auto id=PSXRecompV4::identify_disc(image,"",0,false,false);
        assert(id.opened && id.has_header && id.detected_serial=="SLPS-00998" && id.region=="NTSC-J");
    }
    struct SerialCase {const char* boot;const char* serial;const char* region;};
    const SerialCase serials[]={
        {"SLUSP012.06","SLUS-01206","NTSC-U"},
        {"SLUSP013.46","SLUS-01346","NTSC-U"},
        {"\\folder\\slusp012.06","SLUS-01206","NTSC-U"},
        {"SLUS_005.94","SLUS-00594","NTSC-U"},
        {"SLUS-01206","SLUS-01206","NTSC-U"},
        {"sles_123.45","SLES-12345","PAL"},
        {"SLPS_009.98","SLPS-00998","NTSC-J"},
        {"SLUSP012.0X","",""},
        {"SLUSPP012.06","",""},
        {"ABCDP012.06","",""},
        {"PSX.EXE","",""},
    };
    for(bool raw:{false,true}) {
        const auto image=root/(raw?"disc.bin":"disc.iso");
        for(const auto& entry:serials) {
            fixture(image,raw,entry.boot,false);
            const auto id=PSXRecompV4::identify_disc(image,"",0,false,false);
            assert(id.opened && id.has_header);
            if(id.detected_serial!=entry.serial || id.region!=entry.region)
                std::fprintf(stderr,"raw=%d boot=%s serial=%s region=%s expected=%s/%s\n",
                    raw,entry.boot,id.detected_serial.c_str(),id.region.c_str(),entry.serial,entry.region);
            assert(id.detected_serial==entry.serial && id.region==entry.region);
            if(*entry.serial) {
                assert(PSXRecompV4::identify_disc(image,entry.serial,0,false,false).serial_matches);
                assert(!PSXRecompV4::identify_disc(image,"SLUS-88888",0,false,false).serial_matches);
            }
        }
    }
    fs::remove(root/"disc.iso");fs::remove(root/"disc.bin");fs::remove(root);
}
