#ifndef BAM_AND_READS_H
#define BAM_AND_READS_H

#include <vector>
#include "GenomicRegion.h"
#include "VariantBamReader.h"
#include "MiniRules.h"
#include <time.h>
#include "MinRead.h"

#include "reads.h"

using namespace std;
//using namespace BamTools;

//typedef shared_ptr<BamAlignment> BamAlignmentUP;
//typedef vector<BamAlignmentUP> BamAlignmentUPVector;
//typedef vector<bam1_t*> bam1_v;

//typedef vector<CigarOp> CigarOpVector;
//typedef pair<int32_t, CigarOpVector> CigPos;
//typedef vector<pair<int32_t, CigarOpVector> > CigarPosVec;
//typedef unordered_map<string, CigarPosVec> CigarPosVecMap;
typedef unordered_map<string, size_t> CigarMap;

// a smaller region to assemble
struct AssemblyRegion {

  AssemblyRegion() {}
  AssemblyRegion(GenomicRegion gr) { region = gr; }
  ~AssemblyRegion() {
    //for (vector<BamAlignment*>::iterator it = reads.begin() ; it != reads.end(); ++it) 
    //  delete (*it);
    //reads.clear();
  }
  
  ReadVec reads;
  MinReadSPVector mrv;
  GenomicRegion region;

  GenomicRegionVector partner_windows;
  GenomicIntervalTreeMap tree_pw;

  void removeBlacklist(GenomicIntervalTreeMap &bt);
};

typedef vector<AssemblyRegion> AssemblyRegionVector;
typedef unique_ptr<AssemblyRegion> AssemblyRegionUP;
typedef vector<AssemblyRegionUP> AssemblyRegionUPVector;


// store a reader of a bam file and vectors of pointers
// to reads which live on the heap, for both anchor and partner
struct BamAndReads {

  BamAndReads() {}
  ~BamAndReads() { 
#ifdef HAVE_BAMTOOLS
    delete reader; 
#endif
    arvec.clear();
    //for (AssemblyRegionVector::iterator it = arvec.begin(); it != arvec.end(); it++)
    //  delete (*it);
    //arvec.clear();
  }
  BamAndReads(GenomicRegion gr, MiniRulesCollection *tmr, int tverb, string tbam, string tprefix);

#ifdef HAVE_BAMTOOLS
  BamReader * reader;
#endif

  GenomicRegionVector blacklist;

  void removeBlacklist(GenomicIntervalTreeMap &bt);

  int read_time = 0; // timer in seconds for reading BAM
  int unique_reads = 0; // total number of unique reads to be assembled
  int reads = 0; // total number of reads to be assembled (allows doubles, etc)

  int mate_read_time = 0; // timer in seconds for reading BAM
  int mate_unique_reads = 0; // total number of unique reads to be assembled
  int mate_reads = 0; // total number of reads to be assembled (allows doubles, etc)

#ifdef HAVE_HTSLIB
  // hts
  BGZF * fp = 0;
  hts_idx_t * idx = 0; // hts_idx_load(bamfile.c_str(), HTS_FMT_BAI);
  hts_itr_t * hts_itr = 0; // sam_itr_queryi(idx, 3, 60000, 80000);
  bam_hdr_t * br = 0; // header
  //samFile* fop = 0;
  //fp = sam_open(fn, mode)
#endif  

  // what interval is this defined on
  GenomicRegion interval;
  GenomicIntervalTreeMap tree;

  CigarMap cigmap;

  GenomicRegionVector mate_regions;
  GenomicIntervalTreeMap tree_with_mate;

  // define how to read the bam
  MiniRulesCollection * mr;

  int verbose = 1;
  string bam;
  string prefix;

  // store all of the mini regions to run
  AssemblyRegionUPVector arvec;

  // store all of the discordant reads that map outside of BIGCHUNK
  // we'll need these to lookup extra reads for assembly
  ReadVec disc;

  // break up the interval into small pieces
  void divideIntoRegions();

  // read the bam
  void readBam();

  // read the bam
  void readMateBam();

  // get mate regions
  void calculateMateRegions();

  GenomicRegionVector _read_bam(ReadVec &reads, int limit);

  // use the interval tree to see if a read should be added to a group, based on itself or its mate
  void addRead(Read &r);
  void addMateRead(Read &r);

  friend ostream& operator<<(ostream &out, BamAndReads &bar);
  
};

#endif