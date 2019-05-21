#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <map>
#include <sstream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <elf.h>
#include <libelf.h>

extern "C" {
#include <xed-interface.h>
}

#include "argv_readparam.h"
#include "types.h"
#include "prog.h"
#include "callgraph.h"
#include "cfg.h"
#include "serialize.h"
#include "json_spirit_writer_template.h"

int disassemble(addr_t addr, byte_t *code, addr_t &next1, addr_t &next2, 
		xed_category_enum_t &category, char *buf = NULL, 
		size_t bufsize = 0);

#include "debug.h"

int DEBUG_LEVEL = 2;
FILE *DEBUG_FILE = stderr;

// Cmd line args
static const char *dot = NULL;
static const char *vcg = NULL;
static const char *json = NULL;

Prog the_prog;
const char *prog_name;
typedef std::map<addr_t, Function *> functions_map_t;
functions_map_t functions;

unsigned char *code = 0;
addr_t code_base;
size_t code_size;

void sample_disass(const char *name, addr_t from_addr) {
    printf("%s:\n%08lx: ", name, (long)from_addr);
    for (int i = 0; i < 16; i++) {
	byte_t b = code[from_addr + i - code_base];
	printf("%02x ", b);
    }
    printf("\n");
    addr_t addr = from_addr;
    for (int i = 0; i < 20; i++) {
	char buf[80];
	addr_t next1 = 0, next2 = 0;
	xed_category_enum_t category;

	disassemble(addr, &code[addr - code_base], next1, next2,
		    category, buf, sizeof(buf));
	printf("%08x: %s (0x%08x, 0x%08x)\n", addr, buf, next1, next2);
	addr = next1;
    }
}

Elf *elf;
Elf32_Ehdr *ehdr;

std::unordered_map<addr_t, std::string> addr2name;
std::unordered_map<std::string, addr_t> name2addr;

void read_symtab(Elf32_Shdr *shdr, byte_t *data) {
    int number = shdr->sh_size / shdr->sh_entsize;
    assert(shdr->sh_entsize == sizeof(Elf32_Sym));
    Elf32_Sym *syms = (Elf32_Sym *)data;
    for (int i = 0; i < number; i++) {
        char *name = elf_strptr(elf, shdr->sh_link, syms[i].st_name);
	/* printf("%08x %s (%d)\n", syms[i].st_value, name, strlen(name)); */
	if (name && *name) {
	    std::string sname(name);
	    addr2name[syms[i].st_value] = sname;
	    name2addr[sname] = syms[i].st_value;
	}
    }
}

std::string funcname(addr_t addr) {
    if (addr2name.find(addr) != addr2name.end()) {
	return addr2name[addr];
    } else {
	return std::string("anon");
    }
}

void build_cfg() {
    CallGraph *callgraph = the_prog.getCallGraph();

    // Augment the CFGs of the called functions
    std::set<Function *> worklist;

    // Put all functions in the worklist
    for (functions_map_t::iterator fit = functions.begin(); 
	 fit != functions.end(); fit++) {
	worklist.insert(fit->second);
    }

    while (!worklist.empty()) {
	debug("\n\n----------------------------------------\n\n");
	for (std::set<Function *>::iterator fit = worklist.begin();
	     fit != worklist.end(); fit++) {
	    debug("Statically processing function %.8x %s %d\n", 
		  (*fit)->getAddress(), 
		  funcname((*fit)->getAddress()).c_str(), 
		  (*fit)->isPending());
	    assert(functions.find((*fit)->getAddress()) != functions.end());
	    if ((*fit)->isPending()) {
		(*fit)->setName(funcname((*fit)->getAddress()).c_str());
		(*fit)->setModule(prog_name);
		(*fit)->setProg(&the_prog);
	    }
	    (*fit)->getCfg()->augmentCfg((*fit)->getAddress(), functions);
	    if ((*fit)->isPending()) {
		(*fit)->setPending(false);
		functions[(*fit)->getAddress()] = *fit;
	    }
	}
	
	worklist.clear();
	debug2("Looking for new functions...\n");

	// Put in the worklist all the called functions that have not been
	// visited yet
	for (functions_map_t::iterator fit = functions.begin(); 
	     fit != functions.end(); fit++) {
	    Cfg *cfg = fit->second->getCfg();
	    
	    for (Cfg::const_bb_iterator bbit = cfg->bb_begin(); 
		 bbit != cfg->bb_end(); bbit++) {
		instructions_t::const_iterator iit;
		for (iit = (*bbit)->inst_begin();
		     iit != (*bbit)->inst_end(); iit++) {
		    functions_t::const_iterator ctit;
		    for (ctit = (*iit)->call_targets_begin();
			 ctit != (*iit)->call_targets_end(); ctit++) {
			
			// The function has not been processed yet
			assert(functions.find((*ctit)->getAddress()) != 
			       functions.end());
			callgraph->addCall(fit->second, *ctit);
			if ((*ctit)->isPending()) {
			    worklist.insert(*ctit);
			}
		    }
		}
	    }
	}
    }
}

int main(int argc, char **argv) {
    char *tmpstr;
    const char *cfg_out;

    Elf32_Phdr *phdr;
    Elf32_Shdr *shdr;
    Elf_Scn *scn, *code_scn;
    addr_t start;
    addr_t min_addr = ~0;
    addr_t max_addr = 0;
    unsigned version;

    int fd, res;
    int ph_count;
    int match_count = 0;

    if((tmpstr = argv_getString(argc, argv, "--dot=", NULL)) != NULL ) {
	dot = tmpstr;
    } else {
	dot = NULL;
    }

    if((tmpstr = argv_getString(argc, argv, "--json=", NULL)) != NULL ) {
	json = tmpstr;
    } else {
	json = NULL;
    }

    if((tmpstr = argv_getString(argc, argv, "--vcg=", NULL)) != NULL ) {
	vcg = tmpstr;
    } else {
	vcg = NULL;
    }

    if((tmpstr = argv_getString(argc, argv, "--cfg-out=", NULL)) != NULL ) {
	cfg_out = tmpstr;
    } else {
	cfg_out = NULL;
    }

    if (argc < 2 || argv[argc - 1][0] == '-') {
	fprintf(stderr, "Usage: make-cfg [options] program\n");
	exit(1);
    }

    prog_name = argv[argc - 1];

    fd = open(prog_name, O_RDONLY);
    if (fd == -1) {
	fprintf(stderr, "Failed to open %s for reading: %s\n",
		argv[argc - 1], strerror(errno));
	exit(1);
    }
    
    version = elf_version(EV_CURRENT);
    assert(version != EV_NONE);
    elf = elf_begin(fd, ELF_C_READ, 0);
    ehdr = elf32_getehdr(elf);
    ph_count = ehdr->e_phnum;
    start = ehdr->e_entry;
    phdr = elf32_getphdr(elf);

    /* It might be safer to iterate over segments (the program header)
       instead of sections, but it doesn't look like libelf would provide
       much help with that. */
    scn = 0;
    while ((scn = elf_nextscn(elf, scn)) != 0) {
        char *name;
        shdr = elf32_getshdr(scn);
        name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
	if (shdr->sh_type == SHT_PROGBITS && start >= shdr->sh_addr
	    && start < shdr->sh_addr + shdr->sh_size) {
	    match_count++;
	    printf("Found candidate section at 0x%08x (%s)\n", shdr->sh_addr,
		   name);
	    code_scn = scn;
	}
	min_addr = std::min(min_addr, shdr->sh_addr);
	max_addr = std::max(max_addr, shdr->sh_addr + shdr->sh_size);
    }
    assert(match_count == 1);

    the_prog.addModule(min_addr, max_addr - min_addr, prog_name, true);

    scn = 0;
    while ((scn = elf_nextscn(elf, scn)) != 0) {
        char *name;
	byte_t *data = 0;
        shdr = elf32_getshdr(scn);
        name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
	if (shdr->sh_flags & SHF_ALLOC || shdr->sh_type == SHT_SYMTAB) {
	    data = (byte_t *)calloc(1, shdr->sh_size);
	    if (!data) {
		fprintf(stderr, "Failed to allocate %lu bytes "
			"for %s section, aborting\n",
			(unsigned long)shdr->sh_size, name);
		exit(1);
	    }
	    if (shdr->sh_type == SHT_PROGBITS || shdr->sh_type == SHT_SYMTAB) {
		lseek(fd, shdr->sh_offset, SEEK_SET);
		res = read(fd, data, shdr->sh_size);
		assert(res == (int)shdr->sh_size);
		if (scn == code_scn) {
		    code = data;
		    code_size = shdr->sh_size;
		    code_base = shdr->sh_addr;
		}		    
	    }
	}
	unsigned int flags = 0;
	flags |= 1; /* ELF sections are always readable */
	if (shdr->sh_flags & SHF_WRITE)
	    flags |= (1 << 1);
	if (shdr->sh_flags & SHF_EXECINSTR)
	    flags |= (1 << 2);
	the_prog.addSection(shdr->sh_addr, data, shdr->sh_size, flags, name);
	if (shdr->sh_type == SHT_SYMTAB) {
	    read_symtab(shdr, data);
	}
    }

    assert(code);
    const char *entry_name;
    if (addr2name.find(start) != addr2name.end()) {
	entry_name = addr2name[start].c_str();
    } else {
	entry_name = "_start (fake)";
    }
    Function *func = new Function(entry_name, start, 0, prog_name);   
    functions[start] = func;
    func->setProg(&the_prog);

    /* sample_disass(entry_name, start); */
    build_cfg();

    (void)phdr;
    (void)ph_count;

    elf_end(elf);

    for (functions_map_t::iterator fit = functions.begin(); 
	 fit != functions.end(); fit++) {
	Cfg *cfg = fit->second->getCfg();
	cfg->sanityCheck();
    }

    if (dot || vcg || json) {
	FILE *f;
	char tmp[PATH_MAX];
	CallGraph *callgraph = the_prog.getCallGraph();

	// we want one big file for json, little files for others
	if (json) {
	  json_spirit::Array json_fullcfg;
	  json_spirit::Object json_func, json_cfg;

	  for (functions_map_t::iterator it = functions.begin();
	       it != functions.end(); it++) {
	    Function *func = it->second;
	    json_cfg = func->getCfg()->json();
	    json_fullcfg.push_back(json_cfg);
	  }
	  snprintf(tmp, sizeof(tmp) - 1, "%s/cfg.json",json);
	  tmp[sizeof(tmp) - 1] = '\0';
	  f = fopen(tmp, "w");
	  assert(f);
	  fprintf(f, "%s", 
		  json_spirit::write_string(json_spirit::Value(json_fullcfg), 
					    json_spirit::pretty_print).c_str());
	  fclose(f);
	} else {
	  for (functions_map_t::iterator it = functions.begin(); 
	       it != functions.end(); it++) {
	    Function *func = it->second;
	    if (dot) {
	      snprintf(tmp, sizeof(tmp) - 1, "%s/%.8x.dot", dot, 
		       func->getAddress());
	      tmp[sizeof(tmp) - 1] = '\0';
	      f = fopen(tmp, "w");
	      assert(f);
	      fprintf(f, "%s", func->getCfg()->dot().c_str());
	      fclose(f);
	    }
	    if (vcg) {
	      snprintf(tmp, sizeof(tmp) - 1, "%s/%.8x.vcg", vcg, 
		       func->getAddress());
	      tmp[sizeof(tmp) - 1] = '\0';
	      f = fopen(tmp, "w");
	      if (!f) {
		fprintf(stderr, "Failed to open %s for writing: %s\n",
			tmp, strerror(errno));
		assert(f);
	      }
	      fprintf(f, "%s", func->getCfg()->vcg().c_str());
	      fclose(f);
	    }

	    // Test decoding
	    // func->getCfg()->decode();
	    // Test dominators
	    // func->getCfg()->computeDominators();
	    // Test iterator
	    // for (Cfg::bb_const_iterator bbit = func->getCfg()->bb_begin(); 
	    //      bbit != func->getCfg()->bb_end(); bbit++) {
	    //	debug2("BBIT: %.8x\n", (*bbit)->getAddress());
	    // }
	  }
	}

	if (dot) {
	    snprintf(tmp, sizeof(tmp) - 1, "%s/callgraph.dot", dot);
	    tmp[sizeof(tmp) - 1] = '\0';
	    f = fopen(tmp, "w");
	    assert(f);
	    fprintf(f, "%s", callgraph->dot().c_str());
	    fclose(f);
	}
	if (vcg) {
	    snprintf(tmp, sizeof(tmp) - 1, "%s/callgraph.vcg", vcg);
	    tmp[sizeof(tmp) - 1] = '\0';
	    f = fopen(tmp, "w");
	    assert(f);
	    fprintf(f, "%s", callgraph->vcg().c_str());
	    fclose(f);
	}
    }

    if (cfg_out)
	serialize(cfg_out, the_prog);
}
