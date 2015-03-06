#include "run.h"
#include <iostream>
#include <unordered_map>
#include <stdlib.h>
#include "SnowUtils.h"
#include "api/BamWriter.h"
#include "zfstream.h"
#include "SnowmanAssemble.h"
#include "SnowmanOverlapAlgorithm.h"
#include "SnowmanASQG.h"
#include "faidx.h"
#include "gzstream.h"
#include "vcf.h"
#include <cstdio>

// need for SeqItem
#include "Util.h"

#include <signal.h>
#include <memory>

#define LITTLECHUNK 5000 
#define WINDOW_PAD 300

#define DISC_PAD 400
#define MIN_PER_CLUST 2

#define MATES

using namespace std;
using namespace BamTools;

// BamMap will store filename + type (e.g. /home/mynormalbam.bam, "n1")
typedef unordered_map<string, string> BamMap;

static int read_counter = 0;
static int contig_counter = 0;
static int discordant_counter = 0;
static pthread_mutex_t snow_lock;

static size_t interupt_counter = 0;

// output writers
static BamWriter * r2c_writer;
static ogzstream * all_align_stream;
static ogzstream * os_allbps; 
static ogzstream * contigs_all; 
static ofstream * contigs_sam;
static ogzstream * all_disc_stream;

// reader for converting reference IDs
static BamReader * treader_for_convert;

// bwa index
//static unique_ptr<bwaidx_t> idx;

// detected at least one contig
static bool hashits = false;

namespace opt {

  // assembly parameters
  namespace assemb {
    static unsigned minOverlap = 35;
    static unsigned numBubbleRounds = 3;
    static float divergence = 0.05;
    static float gap_divergence = 0.05;
    static float error_rate = 0.05; 
    static bool writeASQG = false;
    static int maxEdges = 128;
    static int numTrimRounds = 0; //
    static int trimLengthThreshold = -1; // doesn't matter
    static bool bPerformTR = false; // transitivie edge reducetion
    static bool bValidate = false;
    static int resolveSmallRepeatLen = -1; 
    static int maxIndelLength = 20;
    static bool bExact = true;
    static string outVariantsFile = ""; // dummy
  }

  static int32_t readlen;

  // parameters for filtering reads
  static int isize = 800;
  //static string rules = "global@nbases[0,0];!nm[7,1000]!hardclip;!supplementary;!duplicate;!qcfail;phred[4,100];length[50,300]%region@WG%discordant[0,800];mapq[1,100]%mapq[1,1000];clip[5,100]%ins[1,1000]%del[1,1000]";
  static string rules = "global@nbases[0,0];!hardclip;!supplementary;!duplicate;!qcfail;phred[4,100];%region@WG%discordant[0,800];mapq[1,100]%mapq[1,1000];clip[5,100]%ins[1,1000]%del[1,1000]";

  static int chunk = 1000000;

  // runtime parameters
  static unsigned verbose = 1;
  static unsigned numThreads = 1;

  // data
  static BamMap bam;
  static string refgenome = REFHG19;  
  static string analysis_id = "no_id";


  //subsample
  float subsample = 1.0;

  // regions to run
  static std::string regionFile = "x";

  // filters on when / how to assemble
  //static bool normal_assemble = false;
  static bool disc_cluster_only = false;

}

enum { 
  OPT_ASQG,
  OPT_DISC_CLUSTER_ONLY
};

static const char* shortopts = "ht:n:p:v:r:g:r:e:g:k:c:a:";
static const struct option longopts[] = {
  { "help",                    no_argument, NULL, 'h' },
  { "tumor-bam",               required_argument, NULL, 't' },
  { "analysis-id",             required_argument, NULL, 'a' },
  { "normal-bam",              required_argument, NULL, 'n' },
  { "threads",                 required_argument, NULL, 'p' },
  { "chunk-size",              required_argument, NULL, 'c' },
  { "region-file",             required_argument, NULL, 'k' },
  { "rules",                   required_argument, NULL, 'r' },
  { "reference-genome",        required_argument, NULL, 'g' },
  { "write-asqg",              no_argument, NULL, OPT_ASQG   },
  { "error-rate",              required_argument, NULL, 'e'},
  { "verbose",                 required_argument, NULL, 'v' },
  { NULL, 0, NULL, 0 }
};

static const char *RUN_USAGE_MESSAGE =
"Usage: snowman run [OPTION] -t Tumor BAM\n\n"
"  Description: Grab weird reads from the BAM and perform assembly with SGA\n"
"\n"
"  General options\n"
"  -v, --verbose                        Select verbosity level (0-4). Default: 1 \n"
"  -h, --help                           Display this help and exit\n"
"  -p, --threads                        Use NUM threads to run snowman. Default: 1\n"
"  -g, --reference-genome               Path to indexed reference genome to be used by BWA-MEM. Default is Broad hg19 (/seq/reference/...)\n"
"  -a, --analysis-id                    Optionally add a unique identifier for the output files.\n"
"  Required input\n"
"  -t, --tumor-bam                      Tumor BAM file\n"
"  Optional input\n"                       
"  -n, --normal-bam                     Normal BAM file\n"
"  -r, --rules                          VariantBam style rules string to determine which reads to do assembly on. See documentation for default.\n"
"  -m, --min-overlap                    Minimum read overlap, an SGA parameter. Default: 0.4* readlength\n"
"  -k, --region-file                    Set a region txt file. Format: one region per line, Ex: 1,10000000,11000000\n"
"      --disc-cluster-only              Only run the discordant read clustering module, skip assembly. Default: off\n"
"  Assembly params\n"
"      --write-asqg                     Output an ASQG graph file for each 5000bp window. Default: false\n"
"  -e, --error-rate                     Fractional difference two reads can have to overlap. See SGA param. 0 is fast, but requires exact. Default: 0.05\n"
"  -c, --chunk-size                     Amount of genome to read in at once. High numbers have fewer I/O rounds, but more memory. Default 1000000 (1M). Suggested 50000000 (50M) or 'chr' for exomes\n"
"\n";

static struct timespec start;

static MiniRulesCollection * mr;

// handle a ctrl C
void my_handler(int s){

  interupt_counter++;
  if (interupt_counter > 2)
    exit(EXIT_FAILURE);

  if (s > 0)
    printf("\nCaught signal %d. Closing BAMs and ofstreams\n",s);

    contigs_all->close();
  
  if (hashits) {

    r2c_writer->Close();

    if (read_counter < 2000000) {
      //cleanR2CBam();
      string oldf = "r2c_clean.bam";
      string newf = "r2c.bam";
      rename(oldf.c_str(), newf.c_str());
    }
    

    all_align_stream->close();
    all_disc_stream->close();
    os_allbps->close(); 
    contigs_sam->close();

    // convert SAM to BAM
    string cmd = "samtools view contigs.sam -h -Sb > contigs.tmp.bam; samtools sort contigs.tmp.bam contigs; rm contigs.tmp.bam; samtools index contigs.bam; rm contigs.sam";
    cout << cmd << endl;
    system(cmd.c_str());

    // make the VCF file
    if (opt::verbose)
      cout << "...making the VCF files" << endl;

    VCFFile snowvcf("bps.txt.gz", opt::refgenome.c_str(), '\t', opt::analysis_id);
    ogzstream out;
    out.open("vars.vcf.gz", ios::out);
    out << snowvcf;

    // write the indel one
    string basename = opt::analysis_id + ".broad-snowman.DATECODE.";
    snowvcf.writeIndels(basename);
    snowvcf.writeSVs(basename);


  } else {
    cout << "%%%%%%%%%%%%%%%%%%%%" << endl; 
    cout << "NO VARIANTS DETECTED" << endl;
    cout << "%%%%%%%%%%%%%%%%%%%%" << endl; 
  }

  if (s == 0) 
    cout << "******************************" << endl 
	 << "Snowman completed successfully" << endl
         << "******************************" << endl;
  else 
    cout << "Snowman stopped due to signal interrupt, but successfully wrote output files"<< endl;
  SnowUtils::displayRuntime(start);
  cout << endl;
  if (s > 0)
    exit(EXIT_FAILURE); 
  else 
    exit(EXIT_SUCCESS);

}

void sendThreads(GenomicRegionVector &regions_torun) {

  // load the index reference genome onto the heap
  if (opt::verbose > 0)
    cout << "Loading the reference BWT index file for: "  << opt::refgenome << endl;

  unique_ptr<bwaidx_t> idx = unique_ptr<bwaidx_t>(bwa_idx_load(opt::refgenome.c_str(), BWA_IDX_ALL));
  if (idx == NULL) {
    std::cerr << "Could not load the reference: " << opt::refgenome << endl;
    exit(EXIT_FAILURE);
  }

  // Create the queue and consumer (worker) threads
  wqueue<SnowmanWorkItem*>  queue;
  vector<ConsumerThread<SnowmanWorkItem>*> threadqueue;
  for (unsigned i = 0; i < opt::numThreads; i++) {
    ConsumerThread<SnowmanWorkItem>* threadr = new ConsumerThread<SnowmanWorkItem>(queue, opt::verbose > 0);
    threadr->start();
    threadqueue.push_back(threadr);
  }

  // send the jobs
  unsigned count = 0;
  for (GenomicRegionVector::const_iterator it = regions_torun.begin(); it != regions_torun.end(); it++) {
    count++;
    SnowmanWorkItem * item     = new SnowmanWorkItem(it->chr, it->pos1, it->pos2, count, &idx);
    queue.add(item);
  }

  // wait for the threads to finish
    for (unsigned i = 0; i < opt::numThreads; i++) 
    threadqueue[i]->join();

    bwa_idx_destroy(idx.release());

}

// main function to kick-off snowman
bool runSnowman(int argc, char** argv) {

  // start the interrupt handle
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);

  parseRunOptions(argc, argv);

  // initialize the reader for converting chromosomes
  treader_for_convert = new BamReader();
  
  if (!treader_for_convert->Open(opt::bam.begin()->first)) {
    cerr << "Cannot open " << opt::bam.begin()->first << endl;
    exit(EXIT_FAILURE);
  }

  // learn some parameters
  learnParameters();

  // parse the region file, count number of jobs
  GenomicRegionVector file_regions, regions_torun;
  int num_jobs = countJobs(file_regions, regions_torun); 

  // override the number of threads if need
  opt::numThreads = min(num_jobs, static_cast<int>(opt::numThreads));

  if (opt::verbose > 0) {
    cout << "Num threads:         " << opt::numThreads << endl;
    cout << "Assembly error rate: " << opt::assemb::error_rate << endl;
    cout << "Read length: " << opt::readlen << endl;
  }

  if (opt::disc_cluster_only) 
    cout << "RUNNING DISCORDANT CLUSTERING ONLY" << endl;

  if (opt::verbose > 0) {
    for (auto i : opt::bam)
      cout << i.first << " -- " << i.second << endl;
  }

  // set the MiniRules
  mr = new MiniRulesCollection(opt::rules);
  if (opt::verbose > 0)
    cout << *mr;

  // write the tumor header to the contigs SAM file
  contigs_all = new ogzstream("contigs_all.sam.gz", ios::out);
  (*contigs_all) << treader_for_convert->GetHeaderText();

  initializeFiles();

  // open a mutex
  if (pthread_mutex_init(&snow_lock, NULL) != 0) {
      printf("\n mutex init failed\n");
      return false;
  }

  // Create the queue and consumer (worker) threads
//   wqueue<SnowmanWorkItem*>  queue;
//   vector<ConsumerThread<SnowmanWorkItem>*> threadqueue;
//   for (unsigned i = 0; i < opt::numThreads; i++) {
//     ConsumerThread<SnowmanWorkItem>* threadr = new ConsumerThread<SnowmanWorkItem>(queue, opt::verbose > 0);
//     threadr->start();
//     threadqueue.push_back(threadr);
//   }

  // start the timer
  clock_gettime(CLOCK_MONOTONIC, &start);

  // print the jobs
  if (opt::verbose > 0) {
    if (opt::regionFile != "x" ) {
      for (auto& i : file_regions) 
	cout << "Input Regions: " << i << endl;
    }
    else {
      cout << "***Running across whole genome***" << endl;
    }
  }  

  sendThreads(regions_torun);
  
  // close the BAMs
  my_handler(0);


  if (opt::verbose > 0) {
    SnowUtils::displayRuntime(start);
    cout << endl;
  }

  return true;

}

bool grabReads(int refID, int pos1, int pos2, unique_ptr<bwaidx_t>* idx) {

  // place to store the contigs that are made and their read alignments
  AlignedContigVec alc;

  // place to store the reads
  ReadMap this_r2c;

  // count the number of reads
  int num_n_reads = 0, num_t_reads = 0;
  SnowTimer st;

  unique_ptr<BARMap> bar(new BARMap);

  BamQC qc; // debug. This should be a heaped vector

  // TODO make grabReads take GR
  GenomicRegion gr(refID, pos1, pos2);
  gr.pos1 = max(1, gr.pos1);

  st.start();
  // get the regions to run
  GenomicRegionVector grv_small = gr.divideWithOverlaps(5000, 500);
    
  // Open the BAMs and get the reads. only add to first (anchor)
  for (auto& bam : opt::bam) {

    // open a new BAM file at this region
    unique_ptr<BamAndReads> b(new BamAndReads(gr, mr, opt::verbose, bam.first, bam.second));
    //zauto b = make_unique<BamAndReads>(gr, mr, opt::verbose, bam.first, bam.second);
    (*bar)[bam.first] = move(b);
    
    // grab the reads and assign them to assembly regions
    (*bar)[bam.first]->readBam();

    // add the times
    assert(bam.second.length() > 0);
    if (bam.second.at(0) == 'n') {
      num_n_reads += (*bar)[bam.first]->unique_reads;
    } else if (bam.second.at(0) == 't') {
      num_t_reads += (*bar)[bam.first]->unique_reads;
    }
  }

  // make the master blacklist
  GenomicRegionVector master_black;
  for (auto& bam : opt::bam) {      
    master_black.insert(master_black.end(), (*bar)[bam.first]->blacklist.begin(), (*bar)[bam.first]->blacklist.end());
  }
  master_black = GenomicRegion::mergeOverlappingIntervals(master_black);
  GenomicIntervalTreeMap black_tree = GenomicRegion::createTreeMap(master_black);

  //debug
  for (auto& g : master_black)
    cout << "BLACKLIST " << g << endl;
  
  // loop through and remove reads from blacklist
  if (master_black.size())
  for (auto& bam : opt::bam) {
    (*bar)[bam.first]->removeBlacklist(black_tree);
    }


  for (auto& bam : opt::bam) {
    // grab the mate region reads
#ifdef MATES
    (*bar)[bam.first]->calculateMateRegions();
#endif
  }

  st.stop("r");

  // make master normal cigarmap
  CigarMap cigmap_n, cigmap_t;
  for (auto& bam : opt::bam) {
    if (bam.second.at(0) == 'n') {
      for (auto& c : (*bar)[bam.first]->cigmap)
	cigmap_n[c.first] += c.second;
    } else if (bam.second.at(0) == 't') {
      for (auto& c : (*bar)[bam.first]->cigmap)
	cigmap_t[c.first] += c.second;
    }
  }
    

#ifdef MATES

  // merge the mate regions
  /*
  GenomicRegionVector mate_normal, mate_tumor;
  for (auto& bam : opt::bam) {
    if (bam.second.at(0) == 't')
      mate_tumor.insert(mate_tumor.begin(), (*bar)[bam.first]->mate_regions.begin(), (*bar)[bam.first]->mate_regions.end());
    else if (bam.second.at(0) == 'n')
      mate_normal.insert(mate_normal.begin(), (*bar)[bam.first]->mate_regions.begin(), (*bar)[bam.first]->mate_regions.end());      
  }
  mate_normal = GenomicRegion::mergeOverlappingIntervals(mate_normal);
  mate_tumor = GenomicRegion::mergeOverlappingIntervals(mate_tumor);
  size_t mate_region_size = 0;

  // check if the tumor overlaps 
  GenomicRegionVector mate_final;
  for (auto& t : mate_tumor) {
    bool good = true;
    for (auto& n : mate_normal) {
      if (t.getOverlap(n) > 1) {
	good = false;
	break;
      }
    }
    if (good) {
      mate_final.push_back(t);
      mate_region_size += t.width();
    }
  }
  */

  GenomicRegionVector mate_comb;
  for (auto& bam : opt::bam) 
    mate_comb.insert(mate_comb.begin(), (*bar)[bam.first]->mate_regions.begin(), (*bar)[bam.first]->mate_regions.end());
  GenomicRegionVector mate_final = GenomicRegion::mergeOverlappingIntervals(checkReadsMateRegions(mate_comb, bar));
  
  // print it out
  if (opt::verbose > 1) {
    cout << "Mate regions(" << mate_final.size() << ")" << endl;
    if (opt::verbose > 2)
      for (auto &i : mate_final)
	cout << "    " << i << " width " << i.width() << " tcount " << i.tcount << " ncount " << i.ncount << endl;
  }

  // grab the mate reads
  for (auto& bam : opt::bam) {
    (*bar)[bam.first]->mate_regions = mate_final;
    (*bar)[bam.first]->readMateBam();

    // add the times
    assert(bam.second.length() > 0);
    if (bam.second.at(0) == 'n') {
      num_n_reads += (*bar)[bam.first]->mate_unique_reads;
    } else if (bam.second.at(0) == 't') {
      num_t_reads += (*bar)[bam.first]->mate_unique_reads;
    }

    if (opt::verbose > 1) 
      cout << bam.first << " " << *(*bar)[bam.first] << endl;
  }
  st.stop("m");

  // do the discordant read clustering. Put all reads in one vector,
  // and dedupe doubles
  unordered_map<string,bool> dd;


  ReadVec reads_fdisc;
  for (auto& bam : opt::bam) {
    for (auto& v : (*bar)[bam.first]->arvec)
      for (auto& r : v->reads) {
	string tmp;
	r_get_Z_tag(r, "SR", tmp);
	if (dd.count(tmp) == 0) 
	  reads_fdisc.push_back(r);
	dd[tmp] = true;
      }
  }

  //debug
  /*BamWriter writ;
  writ.Open("tmp.bam", treader_for_convert->GetHeaderText(), treader_for_convert->GetReferenceData());
  for (auto& r : reads_fdisc)
    writ.SaveAlignment(*r);
  writ.Close();
  */

  // cluster the discordant reads
  DMap this_disc = clusterDiscordantReads(reads_fdisc);
  st.stop("cl");
  
#endif


  // do the assembly
  // loop through each assembly region. There should be 1 to 1 
  // concordance between grv_small and arvec in each BamAndReads
  for (size_t i = 0; i < grv_small.size(); i++) {
    
    // be extra careful, check for dupes
    // slow-ish, but just in case for weird mate overlaps
    // If not, program crashes in SGA
    unordered_map<string, bool> dupe_check; 
    
    ReadVec bav_join;
    for (auto& bam : *bar) {
      if (bam.second->arvec[i]->reads.size() > 0)
	for (auto& read : bam.second->arvec[i]->reads) {
	  string tmp;
	  r_get_Z_tag(read, "SR", tmp);
	  //read->GetTag("SR", tmp);
	  if (dupe_check.count(tmp) == 1)
	    ;//cerr << "Unexpected duplicate: " << read->RefID << ":" << read->Position << " mate: " << read->MateRefID << ":" << read->MatePosition << " sr: " << tmp << endl;
	  else {
	    dupe_check[tmp] = true;
	    bav_join.push_back(read);
	  }
	}
    }

    // make the reads tables
    ReadTable pRT;
    for (auto& i : bav_join) {
      SeqItem si;
      string sr, seq = "";
      r_get_SR(i, sr);
      r_get_trimmed_seq(i, seq); 
      assert(sr.length());
      assert(seq.length());
      r_get_trimmed_seq(i, seq);

      si.id = sr;
      si.seq = seq;
      if (seq.length() > 40)
	pRT.addRead(si);
    }
    //cout << pRT << endl;

    ContigVector contigs;
    ContigVector contigs1;
    ContigVector contigs0;

    // do the first round of assembly
    string name = "c_" + to_string(grv_small[i].chr+1) + "_" + to_string(grv_small[i].pos1) + "_" + to_string(grv_small[i].pos2);
    if (opt::verbose > 2)
      cout << "Doing assembly on: " << name << " with " << bav_join.size() << " reads" << endl;

    if (bav_join.size() > 1 && bav_join.size() < 10000) {

      // do the first round (on raw reads)
      doAssembly(&pRT, name, contigs0, 0);

      for (size_t yy = 1; yy != 2; yy++) {

	// do the second round (on assembled contigs)
	ReadTable pRTc0(contigs0);
	contigs.clear();
	doAssembly(&pRTc0, name, contigs, yy);      
	contigs0 = contigs;
      }
      
      // de-dupe assemblies
      st.stop("as");

      if (opt::verbose > 2)
	cout << "Doing BWA alignment on: " << name << " with " << contigs.size() << " contigs" << endl;
      
      // peform alignment of contigs to reference with BWA-MEM
      BWAWrapper wrap;
      BWAReadVec bwarv;
      SamRecordVec samv;
      for (auto& i : contigs) 
	bwarv.push_back(BWARead(i.getID(), i.getSeq()));
      wrap.addSequences(bwarv, idx, samv);

      // make aligned contigs from SAM records

      size_t new_alc_start = alc.size();
      for (auto& r : samv) 
	alc.push_back(AlignedContig(r.record, treader_for_convert, grv_small[i]));

      st.stop("bw");     
      // dedupe
      /*
      for (auto& ac : alc) {
	for (auto cc : alc) { 
	  if (ac.m_farbreak == cc.m_farbreak) {
	    if (!ac.isBetter(cc))
	      ac.skip = true;
	  }
	}
      }
      */

      //debug
      //      for (auto& ac : alc) 
      //	cout << ac.skip << endl;

      for (size_t i = new_alc_start; i < alc.size(); i++) {
	if (!alc[i].skip && alc[i].hasVariant() && alc[i].getMinMapq() >= 10 && alc[i].getMaxMapq() >= 40) { 

	  if (alc[i].m_align.size() > 1) { //debug
	    alc[i].alignReadsToContigs(bav_join, false);
	    alc[i].splitCoverage();
	    alc[i].getBreakPairs();
	  } else {
	    alc[i].alignReadsToContigs(bav_join, true);
	    alc[i].splitIndelCoverage();
	    alc[i].indelCigarMatches(cigmap_n, cigmap_t);
	  }
	}
      }
      st.stop("sw");
    } // end bav_join.size() > 1
  } // end the assembly regions loop


  // combine discordant reads with breaks
  combineContigsWithDiscordantClusters(this_disc, alc);
  st.stop("cl");

  // get the breakpoints
  BPVec bp_glob;
  for (auto& i : alc)
    if (i.m_bamreads.size() && !i.skip)
      bp_glob.push_back(i.getGlobalBreak());

  // add discordant reads
  addDiscordantPairsBreakpoints(bp_glob, this_disc);
    
#ifdef MATES
  // add the clusters to the map
  //for (auto& i : this_disc)
  //  (*dmap)[i.first] = i.second;
  discordant_counter += this_disc.size();
#endif
  
  // store the reads in a map of read2contig alignment
  // TODO fancy combine
  for (auto& it : alc) 
    if (!it.skip)
      for (auto& r : it.m_bamreads) 
	this_r2c[to_string(r_flag(r)) + r_qname(r)] = r;
#ifdef MATES
  // add in the reads from the discordant
  for (auto& i : this_disc) { 
    if (i.second.reads_mapq >= 10 && i.second.mates_mapq >= 10) {
      for (auto& r : i.second.reads)
	this_r2c[to_string(r_flag(r.second)) + r_qname(r.second)] = r.second;
      for (auto& r : i.second.mates)
	this_r2c[to_string(r_flag(r.second)) + r_qname(r.second)] = r.second;
    }
  }
#endif
  read_counter += this_r2c.size();
						
  if (!hashits)
  for (auto& i : alc)
    if (i.m_bamreads.size() && !i.skip) {
      hashits = true;
      break;
    }
    
  ////////////////////////////////////
  // MUTEX LOCKED
  ////////////////////////////////////
  // write to the global contig out
  pthread_mutex_lock(&snow_lock);  
  
  // write the r2c
  //writeR2C(this_r2c);

  // send all bps to file
  for (auto& i : bp_glob)
    if (i.hasMinimal())
      (*os_allbps) << i.toFileString() << endl;

  // send all alignments to ASCII plot
  for (auto& i : alc) 
    if (i.m_bamreads.size() && !i.skip && i.hasVariant())
      (*all_align_stream) << i.printAlignments() << endl;

  // send all discordant clusters to txt
  for (auto &i : this_disc)
    if (i.second.reads_mapq >= 10 && i.second.mates_mapq >= 10)
      (*all_disc_stream) << i.second.toFileString() << endl;

  // send all the variant contigs to a sam file
  for (auto& i : alc) {
    if (!i.skip) {
      if (i.m_bamreads.size()) {
	contig_counter++;
	(*contigs_sam) << i.samrecord;
      }
      (*contigs_all) << i.samrecord;
    }
  }
  st.stop("wr");

  // display the run time
  if (opt::verbose > 0) {
    string print1 = SnowUtils::AddCommas<int>(pos1);
    string print2 = SnowUtils::AddCommas<int>(pos2);
    char buffer[140];
    sprintf (buffer, "Ran chr%2s:%11s-%11s | T: %5d N: %5d C: %5d R: %5d D: %5d | ", 
	     treader_for_convert->GetReferenceData()[refID].RefName.c_str(),print1.c_str(),print2.c_str(),
	     num_t_reads, num_n_reads, 
	     contig_counter, read_counter, discordant_counter);
    cout << string(buffer) << st << " | ";
    SnowUtils::displayRuntime(start);
    cout << endl;
  }


  pthread_mutex_unlock(&snow_lock);
  /////////////////////////////////////
  // MUTEX UNLOCKED
  /////////////////////////////////////
 
 return true;
}

// parse the command line options
void parseRunOptions(int argc, char** argv) {
  bool die = false;

  if (argc <= 2) 
    die = true;

  string tmp;
  for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) {
    istringstream arg(optarg != NULL ? optarg : "");
    switch (c) {
      case 'p': arg >> opt::numThreads; break;
      case 'a': arg >> opt::analysis_id; break;
      case 'h': die = true; break;
      case 'c': 
	tmp = "";
	arg >> tmp;
	if (tmp.find("chr") != string::npos) {
	  opt::chunk = 250000000; break;
	} else {
	  opt::chunk = stoi(tmp); break;
	}
    case OPT_ASQG: opt::assemb::writeASQG = true; break;
	case 't': 
	  tmp = "";
	  arg >> tmp;
	  opt::bam[tmp] = "t"; 
	  break;
	case 'n': 
	  tmp = "";
	  arg >> tmp;
	  opt::bam[tmp] = "n"; 
	  break;
      case 'v': arg >> opt::verbose; break;
      case 'i': arg >> opt::isize; break;
      case 'k': arg >> opt::regionFile; break;
      case 'e': arg >> opt::assemb::error_rate; break;
      case 'g': arg >> opt::refgenome; break;
      case 'r': arg >> opt::rules; break;
      case OPT_DISC_CLUSTER_ONLY: opt::disc_cluster_only = true; break;
    }
  }

  // clean the outdir
  //opt::outdir = SnowUtils::getDirPath(opt::outdir);

  // check that we input something
  if (opt::bam.size() == 0) {
    cerr << "Must add a bam file " << endl;
    exit(EXIT_FAILURE);
  }

  // check file validity
  if (opt::regionFile != "x") {
    if (false) {
      cerr << "Region file does not exist: " << opt::regionFile << endl;
      exit(EXIT_FAILURE);
    }
  }
    
  if (opt::numThreads <= 0) {
      cout << "run: invalid number of threads: " << opt::numThreads << "\n";
      die = true;
  }

  //  if (opt::tbam.length() == 0) {
  // cout << "run: must supply a tumor bam"<< "\n";
  //  die = true;
  //}

  if (die) 
    {
      cout << "\n" << RUN_USAGE_MESSAGE;
      exit(1);
    }
}

// just get a count of how many jobs to run. Useful for limiting threads
// also set the regions
int countJobs(GenomicRegionVector &file_regions, GenomicRegionVector &run_regions) {

  // open the region file if it exists
  bool rgfile = opt::regionFile.compare("x") != 0;
  if (rgfile) 
    file_regions = GenomicRegion::regionFileToGRV(opt::regionFile, 0);
  //else if (!opt::ignore_skip_cent)
  //  file_regions = GenomicRegion::non_centromeres; // from GenomicRegion.cpp
  else {
    RefVector ref = treader_for_convert->GetReferenceData();
    size_t dumcount = 0;
    for (auto& i : ref) {
      if (++dumcount <= 25) // go up through mitochondria
	file_regions.push_back(GenomicRegion(treader_for_convert->GetReferenceID(i.RefName), 1, i.RefLength));
    }
  //  file_regions = GenomicRegion::getWholeGenome();
  }

  if (file_regions.size() == 0) {
    cerr << "ERROR: Cannot read region file: " << opt::regionFile << " or something wrong with tumor bam header" << endl;
    exit(EXIT_FAILURE);
  }

  //int threadchunk = opt::chunk;
  unsigned jj = 0; 
  int startr, endr;;
  int kk = 0;
  
  //set amount to modulate
  int thispad = 1000;
  if (rgfile)
    thispad = 0;

  // loop through each region
  bool stoploop = false;
  while (jj < file_regions.size()) {

    // if regions are greater than chunk, breakup
    if ( (file_regions[jj].pos2 - file_regions[jj].pos1) > opt::chunk) {
      startr = max(1,file_regions[jj].pos1-thispad);
      endr = startr + opt::chunk;

      do {
        GenomicRegion grr(file_regions[jj].chr, startr, endr);
		run_regions.push_back(grr);

		if (endr == file_regions[jj].pos2)
	  		stoploop = true;

		kk++;
		endr   = min(file_regions[jj].pos2, (kk+1)*opt::chunk + file_regions[jj].pos1 + thispad);
		startr = min(file_regions[jj].pos2,  kk*opt::chunk + file_regions[jj].pos1);

      } while (!stoploop);

    } else { // region size is below chunk
      run_regions.push_back(file_regions[jj]);
    }
    jj++;
    kk = 0;
    stoploop = false;
  } // end big while

  return run_regions.size();

}

// call the assembler
void doAssembly(ReadTable *pRT, string name, ContigVector &contigs, int pass) {

  if (pRT->getCount() == 0)
    return;

  // forward
  SuffixArray* pSAf = new SuffixArray(pRT, 1, false); //1 is num threads. false is silent/no
  RLBWT *pBWT= new RLBWT(pSAf, pRT);

  // reverse
  pRT->reverseAll();
  SuffixArray * pSAr = new SuffixArray(pRT, 1, false);
  RLBWT *pRBWT = new RLBWT(pSAr, pRT);
  pRT->reverseAll();

  pSAf->writeIndex();
  pSAr->writeIndex();

  double errorRate = opt::assemb::error_rate;
  int min_overlap = opt::assemb::minOverlap;
  if (pass > 0) {
    min_overlap = 50;
    errorRate = 0.05;
  }

  int seedLength = min_overlap;
  int seedStride = seedLength;
  bool bIrreducibleOnly = true; // default

  SnowmanOverlapAlgorithm* pOverlapper = new SnowmanOverlapAlgorithm(pBWT, pRBWT, 
                                                       errorRate, seedLength, 
                                                       seedStride, bIrreducibleOnly);
  pOverlapper->setExactModeOverlap(false);
  pOverlapper->setExactModeIrreducible(false);

  stringstream hits_stream;
  stringstream asqg_stream;

  SnowmanASQG::HeaderRecord headerRecord;
  headerRecord.setOverlapTag(min_overlap);
  headerRecord.setErrorRateTag(errorRate);
  headerRecord.setInputFileTag("");
  headerRecord.setContainmentTag(true); // containments are always present
  headerRecord.setTransitiveTag(!bIrreducibleOnly);
  headerRecord.write(asqg_stream);    

  pRT->setZero();

  size_t workid = 0;
  SeqItem si;

  while (pRT->getRead(si)) {
    SeqRecord read;
    read.id = si.id;
    read.seq = si.seq;
    OverlapBlockList obl;
    OverlapResult rr = pOverlapper->overlapReadInexact(read, min_overlap, &obl);

    pOverlapper->writeOverlapBlocks(hits_stream, workid, rr.isSubstring, &obl);

    SnowmanASQG::VertexRecord record(read.id, read.seq.toString());
    record.setSubstringTag(rr.isSubstring);
    record.write(asqg_stream);

    workid++;
  }

  string line;
  bool bIsSelfCompare = true;
  ReadInfoTable* pQueryRIT = new ReadInfoTable(pRT);

  while(getline(hits_stream, line)) {
    size_t readIdx;
    size_t totalEntries;
    bool isSubstring; 
    OverlapVector ov;
    OverlapCommon::parseHitsString(line, pQueryRIT, pQueryRIT, pSAf, pSAr, bIsSelfCompare, readIdx, totalEntries, ov, isSubstring);
    for(OverlapVector::iterator iter = ov.begin(); iter != ov.end(); ++iter)
    {
       SnowmanASQG::EdgeRecord edgeRecord(*iter);
       edgeRecord.write(asqg_stream);
    }

  }

  // optionally output the graph structure
  if (opt::assemb::writeASQG) {

    // write ASQG to file for visualization
    stringstream asqgfile;
    asqgfile << name << "pass_" << pass << ".asqg";
    ofstream ofile(asqgfile.str(), ios::out);
    ofile << asqg_stream.str();
    ofile.close();

    // write the hits stream file
    stringstream hitsfile;
    hitsfile << name << "pass_" << pass << ".hits";
    ofstream ofile_hits(hitsfile.str(), ios::out);
    ofile_hits << hits_stream.str();
    ofile_hits.close();

   }

    // Get the number of strings in the BWT, this is used to pre-allocated the read table
    delete pOverlapper;
    delete pBWT; 
    delete pRBWT;
    delete pSAf;
    delete pSAr;

    // PERFORM THE ASSMEBLY
    assemble(asqg_stream, min_overlap, opt::assemb::maxEdges, opt::assemb::bExact, 
	     opt::assemb::trimLengthThreshold, opt::assemb::bPerformTR, opt::assemb::bValidate, opt::assemb::numTrimRounds, 
	     opt::assemb::resolveSmallRepeatLen, opt::assemb::numBubbleRounds, opt::assemb::gap_divergence, 
	     opt::assemb::divergence, opt::assemb::maxIndelLength, opt::readlen+1 /*cutoff*/, name + "_", contigs);

    delete pQueryRIT;
    asqg_stream.str("");
    hits_stream.str("");

    // print out some results
    if (opt::verbose > 4) {
      if (contigs.size() >= 1) {
	cout << "Contig Count: " << contigs.size() << " at " << name << endl;
	if (opt::verbose > 3)
	  for (ContigVector::iterator i = contigs.begin(); i != contigs.end(); i++) 
	    cout << "   " << i->getID() << " " << i->getSeq().length() << " " << i->getSeq() << std::endl;
      }
    }


}

/**
 * Merge breaks found by assembly with breaks from discordant clusters
 *
 * Currently, for each AlignedContig, loops through all of the discordant
 * clusters and checks if it overlaps the event. If so, add the cluster
 * to the AlignedContig, and mark the DiscordantCluster as being associated
 * with the AlignedContig.
 *
 * @param dm Map of discordant clusters 
 * @param contigs Vector of AlignedContigs, which store the breakpoints information from assembly
 */
void combineContigsWithDiscordantClusters(DMap &dm, AlignedContigVec &contigs) {

  int padr = 400; 
  size_t count = 0;
  
  for (auto& i : contigs) {
    if (i.m_bamreads.size() && i.m_align.size() > 1 && !i.skip) {
      count++;
      
      // check the global break
      GenomicRegion bp1 = i.m_farbreak.gr1; 
      bp1.pad(padr);
      GenomicRegion bp2 = i.m_farbreak.gr2; 
      bp2.pad(padr);
      
      for (auto &kt : dm) { 
	
	bool bp1reg1 = bp1.getOverlap(kt.second.reg1) != 0;
	bool bp2reg2 = bp2.getOverlap(kt.second.reg2) != 0;
	
	//debug
	bool bp1reg2 = bp1.getOverlap(kt.second.reg2) != 0;
	bool bp2reg1 = bp2.getOverlap(kt.second.reg1) != 0;
	
	// TODO have discordant reads support non global breakpoints
	
	//debug
	//cout << bp1 << " " << bp2 << " bp1reg1: " << bp1reg1 << " bp1reg2: " << bp1reg2 << 
	//	" bp2reg1: " << bp2reg1 << " bp2reg2: " << bp2reg2 << " kt.second.reg1 " << kt.second.reg1 << " kt.second.reg2 " <<  kt.second.reg2 << endl;
	
	bool pass = bp1reg1 && bp2reg2;
	
	//debug
	pass = pass || (bp2reg1 && bp1reg2);
	
	if (pass) {
	  i.addDiscordantCluster(kt.second); // add disc cluster to contig
	  
	  // check that we haven't already added a cluster
	  kt.second.contig = i.getContigName(); // add contig to disc cluster
	  if (i.m_farbreak.dc.reg1.pos1 == 0) {
	    i.m_farbreak.dc = kt.second; // add cluster to global breakpoints
	  } else if (i.m_farbreak.dc.ncount < kt.second.ncount) { // choose one with normal support
	    i.m_farbreak.dc = kt.second;
	  } else if (i.m_farbreak.dc.tcount < kt.second.tcount) { // choose one with more tumor support
	    i.m_farbreak.dc = kt.second;
	  }
	}
	
      }
    }
  }
}

// transfer discordant pairs not mapped to a split break 
void addDiscordantPairsBreakpoints(BPVec &bp, DMap &dmap) {

  if (opt::verbose > 1)
    cout << "...transfering discordant clusters to breakpoints structure" << endl;
  for (auto& i : dmap) { 
    if (i.second.contig == "" && (i.second.tcount+i.second.ncount) > 1) { // this discordant cluster isn't already associated with a break
      BreakPoint tmpbp(i.second);
      bp.push_back(tmpbp);
    }
  }
}

/**
 * Write the read support BAM file.
 *
 * Sorts the reads that are supplied by coordinate and adds them to the open 
 * BAM file. This method does not ensure that reads are not duplicated
 * within the BAM, as it is writing to an already open bam. See cleanR2CBam.
 *
 * @param  r2c Map of the reads to write
 */
/*
void writeR2C(ReadMap &r2c) {

  if (r2c.size() == 0)
    return; 

  // sort by position
  ReadVec r2c_vec;
  for (auto& r : r2c)
    r2c_vec.push_back(r.second);
  sort(r2c_vec.begin(), r2c_vec.end(), ByReadPosition());

  // add the reads
  for (auto& r : r2c_vec) {
    r2c_writer->SaveAlignment(*r);
  }

}
*/

// cluster the discordant reads
DMap clusterDiscordantReads(ReadVec &bav) {

  // remove any reads that are not present twice
  unordered_map<string, int> tmp_map;
  for (auto& i : bav)
    if (tmp_map.count(r_qname(i)) ==0)
      tmp_map[r_qname(i)] = 1;
    else
      tmp_map[r_qname(i)]++;
  ReadVec bav_dd;
  for (auto&i : bav)
    if (tmp_map[r_qname(i)] == 2)
      bav_dd.push_back(i);

  // sort by position
  sort(bav_dd.begin(), bav_dd.end(), ByReadPosition());

  //debug
  //for (auto& i : bav_dd)
  //  cout << r_id(i) << ":" << r_pos(i) << endl;

  // clear the tmp map. Now we want to use it to store if we already clustered read
  tmp_map.clear();

  vector<ReadVec> fwd, rev, fwdfwd, revrev, fwdrev, revfwd;
  ReadVec this_fwd, this_rev;

  pair<int, int> fwd_info, rev_info; // refid, pos
  fwd_info = {-1,-1};
  rev_info = {-1,-1};

  // cluster in the READ direction
  for (auto& i : bav_dd) {
    if (r_is_pmapped(i) && abs(r_isize(i)) >= opt::isize && tmp_map.count(r_qname(i)) == 0) {
      tmp_map[r_qname(i)] = 0;
      // forward clustering
      if (!r_is_rstrand(i)) 
	_cluster(fwd, this_fwd, i, false);
      // reverse clustering 
      else 
      	_cluster(rev, this_rev, i, false);
    }
  }
  // finish the last clusters
  if (this_fwd.size() > 0)
    fwd.push_back(this_fwd);
  if (this_rev.size() > 0)
    rev.push_back(this_rev);

  // cluster in the MATE direction for FWD facing READ
  for (auto& v : fwd) {
    fwd_info = {-1,-1};
    rev_info = {-1,-1};
    this_fwd.clear(); this_rev.clear();
    // sort by mate position to prepare for second clustering
    sort(v.begin(), v.end(), ByMatePosition());
    for (auto& i : v) {
      // forward clustering
      if (!r_is_mrstrand(i)) 
	_cluster(fwdfwd, this_fwd, i, true);
      // reverse clustering 
      else 
	_cluster(fwdrev, this_rev, i, true);
    }
    // finish the last clusters
    if (this_fwd.size() > 0)
      fwdfwd.push_back(this_fwd);
    if (this_rev.size() > 0)
      fwdrev.push_back(this_rev);
  }

  // cluster in the MATE direction for REV facing READ
  for (auto& v : rev) {
    fwd_info = {-1,-1};
    rev_info = {-1,-1};
    this_fwd.clear(); this_rev.clear();
    // sort by mate position to prepare for second clustering
    sort(v.begin(), v.end(), ByMatePosition());
    for (auto& i : v) {
      if (!r_is_mrstrand(i) )
	_cluster(revfwd, this_fwd, i, true);
      // reverse clustering 
      else 
	_cluster(revrev, this_rev, i, true);
    }
    // finish the last clusters
    if (this_fwd.size() > 0)
      revfwd.push_back(this_fwd);
    if (this_rev.size() > 0)
      revrev.push_back(this_rev);
  }    

  DMap dd;
  _convertToDiscordantCluster(dd, fwdfwd, bav_dd);
  _convertToDiscordantCluster(dd, fwdrev, bav_dd);
  _convertToDiscordantCluster(dd, revfwd, bav_dd);
  _convertToDiscordantCluster(dd, revrev, bav_dd);

  // print the clusters
  if (opt::verbose > 1) {
    for (auto& i : dd)
      cout << i.second << endl;
  }
  
  return dd;

}

// is this a read from a tumor
bool isTumorRead(const Read &a) {

  string tmp;
  r_get_SR(a, tmp);
  return tmp.at(0) == 't';


}

/**
 * Cluster reads by alignment position 
 * 
 * Checks whether a read belongs to a cluster. If so, adds it. If not, ends
 * and stores cluster, adds a new one.
 *
 * @param cvec Stores the vector of clusters, which themselves are vectors of read pointers
 * @param clust The current cluster that is being added to
 * @param a Read to add to cluster
 * @param mate Flag to specify if we should cluster on mate position instead of read position
 * @return Description of the return value
 */
bool _cluster(vector<ReadVec> &cvec, ReadVec &clust, Read &a, bool mate) {

  // get the position of the previous read. If none, we're starting a new one so make a dummy
  pair<int,int> last_info;
  if (clust.size() == 0)
    last_info = {-1, -1};
  else if (mate)
    last_info = {r_mid(clust.back()), r_mpos(clust.back())};
  else
    last_info = {r_id(clust.back()), r_pos(clust.back())};

  // get the position of the current read
  pair<int,int> this_info;
  if (mate)
    this_info = {r_mid(a), r_mpos(a)};
  else 
    this_info = {r_id(a), r_pos(a)};

  // check if this read is close enough to the last
  if ( (this_info.first == last_info.first) && (this_info.second - last_info.second) <= DISC_PAD ) {
    clust.push_back(a);
    last_info = this_info;
    return true;
  // read does not belong to cluster. close this cluster and add to cvec
  } else {

    // if enough supporting reads, add as a cluster
    if (clust.size() >= MIN_PER_CLUST) 
      cvec.push_back(clust);

    // clear this cluster and start a new one
    clust.clear();
    clust.push_back(a);

    return false;
  }

}

/**
 * Cluster reads by alignment position 
 * 
 * Checks whether a read belongs to a cluster. If so, adds it. If not, ends
 * and stores cluster, adds a new one.
 *
 * @param cvec Stores the vector of clusters, which themselves are vectors of read pointers
 * @param clust The current cluster that is being added to
 * @param a Read to add to cluster
 * @param mate Flag to specify if we should cluster on mate position instead of read position
 * @return Description of the return value
 */
/*
bool _cluster(vector<bam1_v> &cvec, bam1_v &clust, bam1_t *b, bool mate) {

  // get the position of the previous read. If none, we're starting a new one so make a dummy
  pair<int,int> last_info;
  if (clust.size() == 0)
    last_info = {-1, -1};
  else if (mate)
    last_info = {clust.back()->core.mtid, clust.back()->core.mtid};
  else
    last_info = {clust.back()->core.tid, clust.back()->core.pos};

  // get the position of the current read
  pair<int,int> this_info;
  if (mate)
    this_info = {b->core.mtid, b->core.mpos};
  else 
    this_info = {b->core.tid, b->core.pos};

  // check if this read is close enough to the last
  if ( (this_info.first == last_info.first) && (this_info.second - last_info.second) <= DISC_PAD ) {
    clust.push_back(b);
    last_info = this_info;
    return true;
  // read does not belong to cluster. close this cluster and add to cvec
  } else {

    // if enough supporting reads, add as a cluster
    if (clust.size() >= MIN_PER_CLUST) 
      cvec.push_back(clust);

    // clear this cluster and start a new one
    clust.clear();
    clust.push_back(a);

    return false;
  }

}
*/

/**
 * Remove / combine duplicate reads in read support BAM.
 *
 * To save memory, the supporting read BAM is written on-the-fly.
 * This means that the final BAM has duplicates and is unsorted.
 * This reads in the r2c.bam file and de-duplicates by combining
 * the AL, CN, SW and DC tags with an 'x' separation. Writes the 
 * r2c_clean.bam file.
 */
/*
void cleanR2CBam() {

  if (opt::verbose)
    cout << "...cleaning R2C bam" << endl;

  // open the unsorted / uncleaned bam
  BamReader br;
  if (!br.Open("r2c.bam")) {
    cerr << "Cannot open r2c.bam" << endl;
    return;
  }

  // open a BAM for writing
  BamWriter bw;
  if (!bw.Open("r2c_clean.bam", br.GetHeaderText(), br.GetReferenceData())) {
    cerr << "Cannot open r2c_clean.bam" << endl;
    return;
  }

  unordered_map<string, Read> map;
  ReadVec vec;

  BamAlignment a;
  while (br.GetNextAlignment(a)) {
    string tmp;
    a.GetTag("SR",tmp);
    if (map.count(tmp) == 0) {
      Read b(new BamAlignment(a));
      map[tmp] = b;
    } else {
      
      string cn, dc, sw, al;
      a.GetTag("CN",cn);
      a.GetTag("DC",dc);
      a.GetTag("SW",sw);
      a.GetTag("AL",al);

      SnowUtils::SmartAddTag(map[tmp],"CN",cn);
      SnowUtils::SmartAddTag(map[tmp],"AL",al);
      SnowUtils::SmartAddTag(map[tmp],"SW",sw);
      SnowUtils::SmartAddTag(map[tmp],"DC",dc);

    }
  }

  // transfer to vector and sort
  for (auto& r : map)
    vec.push_back(r.second);
  sort(vec.begin(), vec.end(), ByReadPosition());

  // write it
  for (auto& r : vec)
    bw.SaveAlignment(*r);
  bw.Close();

  // index it
  br.Close();
  if (!br.Open("r2c_clean.bam")) {
    cerr << "Cannot open cleaned r2c_clean.bam" << endl;
    return;
  }
  br.CreateIndex();

  string cmd = "rm r2c.bam;";
  system(cmd.c_str());

}
*/
// convert a cluster of reads into a new DiscordantCluster object
void _convertToDiscordantCluster(DMap &dd, vector<ReadVec> cvec, ReadVec &bav) {

  for (auto& v : cvec) {
    if (v.size() > 1) {
      DiscordantCluster d(v, bav); /// slow but works
      dd[d.id] = d;
    }
  }

}

// convert a cluster of reads into a new DiscordantCluster object
/*void _convertToDiscordantCluster(DMap &dd, vector<bam1_v> cvec, bam1_v &bav) {

  for (auto& v : cvec) {
    DiscordantCluster d(v, bav); /// slow but works
    dd[d.id] = d;
  }

  }*/


/**
 * Open the output files for writing
 */
void initializeFiles() {

  // setup the ASCII plots
  all_align_stream = new ogzstream("alignments.txt.gz", ios::out);
  all_disc_stream  = new ogzstream("discordant.txt.gz", ios::out);
  os_allbps        = new ogzstream("bps.txt.gz", ios::out);
  contigs_sam      = new ofstream("contigs.sam", ios::out);
  
  // write the bp header
  (*os_allbps) << BreakPoint::header() << endl;
  
  // write the discordant reads header
  (*all_disc_stream) << DiscordantCluster::header() << endl;

  // write the tumor header to the contigs SAM file
  (*contigs_sam) << treader_for_convert->GetHeaderText();
  
  // setup the r2c bam writer
  r2c_writer = new BamWriter();
  
  // set the r2c writer
  if (!r2c_writer->Open("r2c.bam", treader_for_convert->GetHeaderText(), treader_for_convert->GetReferenceData())) {
    cerr << "Could not open the r2c.bam for reading." << endl;
    exit(EXIT_FAILURE);
  }
}

//
void learnParameters() {

  BamAlignment a;
  size_t count = 0;
  while (treader_for_convert->GetNextAlignmentCore(a) && count++ < 10000) 
    opt::readlen = max(a.Length, opt::readlen);
  treader_for_convert->Rewind();
 
}


// check reads against mate regions
GenomicRegionVector checkReadsMateRegions(GenomicRegionVector mate_final, unique_ptr<BARMap>& bar) {

  GenomicRegionVector new_mate_final;

  for (auto& i : mate_final) {
    for (auto& bam : opt::bam) {
      for (auto& v : (*bar)[bam.first]->arvec) {
	for (auto& r : v->reads) {
	  if (i.getOverlap(GenomicRegion(r_mid(r), r_mpos(r), r_mpos(r))) > 0) {
	    if (bam.second.at(0) == 'n') 
	      i.ncount++;
	    else
	      i.tcount++;
	  }
	}
      }
    }
    if (opt::verbose > 3)
      cout << "MATE REGION: " << i << " tcount " << i.tcount << " ncount " << i.ncount << endl;
    if (i.ncount == 0 && i.tcount >= 3)
      new_mate_final.push_back(i);
  }

  return new_mate_final;
}



/*
// cluster the discordant reads
DMap clusterDiscordantReads(bam1_v &bav) {

  vector<string> names;
  for (int i = 0; i < bav.size(); i++)
    names.push_back(string(bam_get_qname(bav[i])));

  // remove any reads that are not present twice
  unordered_map<string, int> tmp_map;
  for (int i = 0; i < bav.size(); i++) // auto& i : bav) 
    if (tmp_map.count(names[i]) ==0)
      tmp_map[names[i]] = 1;
    else
      tmp_map[names[i]]++;
  //ReadVec bav_dd;
  bav1_v bav_dd;
  for (int i = 0; i < bav.size(); i++) 
    if (tmp_map[names[i]] == 2)
      bav_dd.push_back(bav[i]);

  // sort by position
  sort(bav_dd.begin(), bav_dd.end(), ByReadPositionB());

  // clear the tmp map. Now we want to use it to store if we already clustered read
  tmp_map.clear();

  //vector<ReadVec> fwd, rev, fwdfwd, revrev, fwdrev, revfwd;
  //ReadVec this_fwd, this_rev;
  vector<bam1_v> fwd, rev, fwdfwd, revrev, fwdrev, revfwd;
  bam1_v this_fwd, this_rev;

  pair<int, int> fwd_info, rev_info; // refid, pos
  fwd_info = {-1,-1};
  rev_info = {-1,-1};

  // cluster in the READ direction
  for (int i = 0; i < bav_dd.size(); i++) {
    //if (i->IsMapped() && i->IsMateMapped() && abs(i->InsertSize) >= opt::isize && tmp_map.count(i->Name) == 0) {
    bool mapped_pair = (!bav_dd[i]->core.flag&BAM_FUNMAP) && (!bav_dd[i]->core.flag&BAM_FMUNMAP);
    if (mapped_pair && abs(i->core.isize) >= opt::isize && tmp_map.count(names[i]) == 0) {
      tmp_map[names[i]] = 0;
      // forward clustering
      if (!bam_is_rev(bav_dd[i])) 
	_cluster(fwd, this_fwd, i, false);
      // reverse clustering 
      else 
      	_cluster(rev, this_rev, i, false);
    }
  }
  // finish the last clusters
  if (this_fwd.size() > 0)
    fwd.push_back(this_fwd);
  if (this_rev.size() > 0)
    rev.push_back(this_rev);

  // cluster in the MATE direction for FWD facing READ
  for (auto& v : fwd) {
    fwd_info = {-1,-1};
    rev_info = {-1,-1};
    this_fwd.clear(); this_rev.clear();
    // sort by mate position to prepare for second clustering
    sort(v.begin(), v.end(), ByMatePositionB());
    for (auto& i : v) {
      // forward clustering
      if (!bam_is_mrev(i)) 
	_cluster(fwdfwd, this_fwd, i, true);
      // reverse clustering 
      else 
	_cluster(fwdrev, this_rev, i, true);
    }
    // finish the last clusters
    if (this_fwd.size() > 0)
      fwdfwd.push_back(this_fwd);
    if (this_rev.size() > 0)
      fwdrev.push_back(this_rev);
  }

  // cluster in the MATE direction for REV facing READ
  for (auto& v : rev) {
    fwd_info = {-1,-1};
    rev_info = {-1,-1};
    this_fwd.clear(); this_rev.clear();
    // sort by mate position to prepare for second clustering
    sort(v.begin(), v.end(), ByMatePositionB());
    for (auto& i : v) {
      if (!bam_is_mrev(i)) 
	_cluster(revfwd, this_fwd, i, true);
      // reverse clustering 
      else 
	_cluster(revrev, this_rev, i, true);
    }
    // finish the last clusters
    if (this_fwd.size() > 0)
      revfwd.push_back(this_fwd);
    if (this_rev.size() > 0)
      revrev.push_back(this_rev);
  }    

  DMap dd;
  _convertToDiscordantCluster(dd, fwdfwd, bav_dd);
  _convertToDiscordantCluster(dd, fwdrev, bav_dd);
  _convertToDiscordantCluster(dd, revfwd, bav_dd);
  _convertToDiscordantCluster(dd, revrev, bav_dd);

  // print the clusters
  if (opt::verbose > 1) {
    for (auto& i : dd)
      cout << i.second << endl;
  }
  
  return dd;

}
*/



/**
 *
 */
/*
SeqRecordVector toSeqRecordVector(ReadVec &bav) {

  SeqRecordVector srv;

  for (auto &i : bav) {

    SeqItem si;
    r_get_Z_tag(i, "SR", si.id);
    assert(si.id.length());
    
    string seqr;
    r_get_trimmed_seq(i, seqr);

    si.seq = seqr;
    assert(seqr.length());
    srv.push_back(si);
  }
}
*/  