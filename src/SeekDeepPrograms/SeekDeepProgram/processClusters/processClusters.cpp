//
// SeekDeep - A library for analyzing amplicon sequence data
// Copyright (C) 2012-2019 Nicholas Hathaway <nicholas.hathaway@umassmed.edu>,
// Jeffrey Bailey <Jeffrey.Bailey@umassmed.edu>
//
// This file is part of SeekDeep.
//
// SeekDeep is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SeekDeep is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SeekDeep.  If not, see <http://www.gnu.org/licenses/>.
//
//
//  main.cpp
//  SeekDeep
//
//  Created by Nicholas Hathaway on 8/11/13.
//
#include "SeekDeepPrograms/SeekDeepProgram/SeekDeepRunner.hpp"



namespace njhseq {


std::unordered_map<std::string, double> processCustomCutOffs(const bfs::path & customCutOffsFnp, const VecStr & allSamples, double defaultFracCutOff){
	std::unordered_map<std::string, double> ret;
	if ("" != customCutOffsFnp.string()) {
		table customCutOffsTab(customCutOffsFnp.string(), "\t", true);
		customCutOffsTab.checkForColumnsThrow(VecStr{"sample", "cutOff"}, __PRETTY_FUNCTION__);
		for (const auto & rowPos : iter::range(customCutOffsTab.content_.size())) {
			ret[customCutOffsTab.content_[rowPos][customCutOffsTab.getColPos(
					"sample")]] =
					njh::lexical_cast<double>(
							customCutOffsTab.content_[rowPos][customCutOffsTab.getColPos(
									"cutOff")]);
		}
	}
	for(const auto & samp : allSamples){
		if(!njh::in(samp, ret)){
			ret[samp] = defaultFracCutOff;
		}
	}
	return ret;
}




int SeekDeepRunner::processClusters(const njh::progutils::CmdArgs & inputCommands) {
	// parameters
	SeekDeepSetUp setUp(inputCommands);
	processClustersPars pars;
	setUp.setUpMultipleSampleCluster(pars);
	// start a run log
	setUp.startARunLog(setUp.pars_.directoryName_);
	// parameters file
	setUp.writeParametersFile(setUp.pars_.directoryName_ + "parametersUsed.txt",
			false, false);
	//write clustering parameters
	auto parsDir = njh::files::makeDir(setUp.pars_.directoryName_, njh::files::MkdirPar("pars"));
	std::ofstream parsOutFile;
	openTextFile(parsOutFile, OutOptions(njh::files::make_path(parsDir, "pars.tab.txt")));
	pars.iteratorMap.writePars(parsOutFile);
	std::ofstream popParsOutFile;
	openTextFile(popParsOutFile, OutOptions(njh::files::make_path(parsDir, "popPars.tab.txt")));
	pars.popIteratorMap.writePars(popParsOutFile);


	//population seqs;
	std::vector<seqInfo> globalPopSeqs;
	if("" != pars.popSeqsFnp){
		globalPopSeqs = SeqInput::getSeqVec<seqInfo>(SeqIOOptions::genFastaIn(pars.popSeqsFnp));
	}

	//read in the files in the corresponding sample directories
	auto analysisFiles = njh::files::listAllFiles(pars.masterDir, true,
			{ std::regex { "^" + setUp.pars_.ioOptions_.firstName_.string() + "$" } }, 3);

	std::set<std::string> samplesDirsSet;
	for (const auto & af : analysisFiles) {
		auto fileToks = njh::tokenizeString(bfs::relative(af.first, pars.masterDir).string(), "/");
		if (3 != fileToks.size()) {
			std::stringstream ss;
			ss << "File path should be three levels deep, not " << fileToks.size()
					<< " for " << bfs::relative(af.first, pars.masterDir).string() << std::endl;
			throw std::runtime_error { ss.str() };
		}
		if(njh::in(fileToks[0], pars.excludeSamples)){
			continue;
		}
		samplesDirsSet.insert(fileToks[0]);
	}

	VecStr samplesDirs(samplesDirsSet.begin(), samplesDirsSet.end());
	VecStr specificFiles;

	for (const auto& fileIter : analysisFiles) {
		specificFiles.push_back(fileIter.first.string());
	}
	if(setUp.pars_.verbose_){
		std::cout << "Reading from" << std::endl;
		for (const auto& sfIter : specificFiles) {
			std::cout << sfIter << std::endl;
		}
	}
	// reading in reads
	uint64_t maxSize = 0;
	// reading expected sequences to compare to
	bool checkingExpected = setUp.pars_.refIoOptions_.firstName_ != "";
	std::vector<readObject> expectedSeqs;
	if (checkingExpected) {
		expectedSeqs = SeqInput::getReferenceSeq(setUp.pars_.refIoOptions_, maxSize);
	}
	// get max size for aligner
	for (const auto& sf : specificFiles) {
		SeqIOOptions inOpts(sf, setUp.pars_.ioOptions_.inFormat_, true);
		SeqInput reader(inOpts);
		reader.openIn();
		seqInfo seq;
		while(reader.readNextRead(seq)){
			readVec::getMaxLength(seq, maxSize);
		}
	}
	// create aligner class object
	aligner alignerObj(maxSize, setUp.pars_.gapInfo_, setUp.pars_.scoring_,
			KmerMaps(setUp.pars_.colOpts_.kmerOpts_.kLength_),
			setUp.pars_.qScorePars_, setUp.pars_.colOpts_.alignOpts_.countEndGaps_,
			setUp.pars_.colOpts_.iTOpts_.weighHomopolyer_);
	alignerObj.processAlnInfoInput(setUp.pars_.alnInfoDirName_);



	// create collapserObj used for clustering
	collapser collapserObj(setUp.pars_.colOpts_);
	collapserObj.opts_.kmerOpts_.checkKmers_ = false;
	// output info about the read In reads
	collapse::SampleCollapseCollection sampColl(setUp.pars_.ioOptions_, pars.masterDir,
			setUp.pars_.directoryName_,
			PopNamesInfo(pars.experimentName, samplesDirs),
			pars.preFiltCutOffs);



	if("" != pars.groupingsFile){
		sampColl.addGroupMetaData(pars.groupingsFile);
	}

	//process custom cut offs
	std::unordered_map<std::string, double> customCutOffsMap = processCustomCutOffs(pars.customCutOffs, samplesDirs, pars.fracCutoff);

	{
		njh::concurrent::LockableQueue<std::string> sampleQueue(samplesDirs);
		njhseq::concurrent::AlignerPool alnPool(alignerObj, pars.numThreads);
		alnPool.initAligners();
		alnPool.outAlnDir_ = setUp.pars_.outAlnInfoDirName_;

		auto setupClusterSamples = [&sampleQueue, &alnPool,&collapserObj,&pars, &setUp,&expectedSeqs,&sampColl,&customCutOffsMap](){
			std::string samp = "";
			auto currentAligner = alnPool.popAligner();
			while(sampleQueue.getVal(samp)){
				if(setUp.pars_.verbose_){
					std::cout << "Starting: " << samp << std::endl;
				}

				sampColl.setUpSample(samp, *currentAligner, collapserObj, setUp.pars_.chiOpts_);

				sampColl.clusterSample(samp, *currentAligner, collapserObj, pars.iteratorMap);

				sampColl.sampleCollapses_.at(samp)->markChimeras(pars.chiCutOff);

				//exclude clusters that don't have the necessary replicate number
				//defaults to the number of input replicates if none supplied
				if (0 != pars.runsRequired) {
					sampColl.sampleCollapses_.at(samp)->excludeBySampNum(pars.runsRequired, true);
				} else {
					sampColl.sampleCollapses_.at(samp)->excludeBySampNum(sampColl.sampleCollapses_.at(samp)->input_.info_.infos_.size(), true);
				}

				sampColl.excludeOnFrac(samp, customCutOffsMap, pars.fracExcludeOnlyInFinalAverageFrac);

				if(pars.collapseLowFreqOneOffs){
					sampColl.sampleCollapses_.at(samp)->excludeLowFreqOneOffs(true, pars.lowFreqMultiplier, *currentAligner);
				}


				if (!pars.keepChimeras) {
					//now exclude all marked chimeras, currently this will also remark chimeras unnecessarily
					sampColl.sampleCollapses_.at(samp)->excludeChimerasNoReMark(true);
				}

				std::string sortBy = "fraction";
				sampColl.sampleCollapses_.at(samp)->renameClusters(sortBy);


				if (!expectedSeqs.empty()) {
					sampColl.sampleCollapses_.at(samp)->excluded_.checkAgainstExpected(
							expectedSeqs, *currentAligner, false);
					sampColl.sampleCollapses_.at(samp)->collapsed_.checkAgainstExpected(
							expectedSeqs, *currentAligner, false);
					if(setUp.pars_.debug_){
						std::cout << "sample: " << samp << std::endl;
					}
					for(const auto & clus : sampColl.sampleCollapses_.at(samp)->collapsed_.clusters_){
						if(setUp.pars_.debug_){
							std::cout << clus.seqBase_.name_ << " : " << clus.expectsString << std::endl;
						}
						if("" ==  clus.expectsString ){
							std::stringstream ss;
							ss << __PRETTY_FUNCTION__ << ": Error, expects string is blank" << std::endl;
							ss << clus.seqBase_.name_ << std::endl;
							throw std::runtime_error{ss.str()};
						}
					}
				}

				sampColl.dumpSample(samp);

				if(setUp.pars_.verbose_){
					std::cout << "Ending: " << samp << std::endl;
				}
			}
		};
		std::vector<std::thread> threads;
		for(uint32_t t = 0; t < pars.numThreads; ++t){
			threads.emplace_back(std::thread(setupClusterSamples));
		}
		for(auto & t : threads){
			t.join();
		}
	}

	//read in the dump alignment cache
	alignerObj.processAlnInfoInput(setUp.pars_.alnInfoDirName_);

//	if (pars.investigateChimeras) {
//		sampColl.investigateChimeras(pars.chiCutOff, alignerObj);
//	}




	if(setUp.pars_.verbose_){
		std::cout << njh::bashCT::boldGreen("Pop Clustering") << std::endl;
	}
	if(!pars.noPopulation){
		sampColl.doPopulationClustering(sampColl.createPopInput(),
				alignerObj, collapserObj, pars.popIteratorMap);

		if(pars.rescueExcludedChimericHaplotypes || pars.rescueExcludedOneOffLowFreqHaplotypes){
			//firth gather major haplotypes
			std::set<std::string> majorHaps;
			for(const auto & sampleName : sampColl.passingSamples_){
				sampColl.setUpSampleFromPrevious(sampleName);
				auto sampPtr = sampColl.sampleCollapses_.at(sampleName);
				for(uint32_t clusPos = 0; clusPos < 2 && clusPos < sampPtr->collapsed_.clusters_.size(); ++clusPos){
					if(sampPtr->collapsed_.clusters_[clusPos].seqBase_.frac_ >= pars.majorHaplotypeFracForRescue){
						majorHaps.emplace(sampColl.popCollapse_->collapsed_.clusters_[sampColl.popCollapse_->collapsed_.subClustersPositions_.at(sampPtr->collapsed_.clusters_[clusPos].getStubName(true))].seqBase_.name_);
					}
				}
				sampColl.dumpSample(sampleName);
			}
			if(setUp.pars_.debug_){
				std::cout << "majorHaps: " << njh::conToStr(majorHaps, ",") << std::endl;
			}
			bool rescuedHaplotypes = false;
			for(const auto & sampleName : sampColl.passingSamples_){
				sampColl.setUpSampleFromPrevious(sampleName);
				auto sampPtr = sampColl.sampleCollapses_.at(sampleName);
				std::vector<uint32_t> toBeRescued;
				//iterator over haplotypes, determine if they should be considered for rescue, if they should be then check to see if they match a major haplotype
				for(const auto excludedPos : iter::range(sampPtr->excluded_.clusters_.size())){
					const auto & excluded = sampPtr->excluded_.clusters_[excludedPos];
					if(excluded.nameHasMetaData()){
						MetaDataInName excludedMeta(excluded.seqBase_.name_);
						std::set<std::string> otherExcludedCriteria;
						bool chimeriaExcludedRescue = false;
						bool oneOffExcludedRescue = false;
						for(const auto & excMeta : excludedMeta.meta_){
							if(njh::beginsWith(excMeta.first, "Exclude") ){
								bool other = true;
								if(pars.rescueExcludedChimericHaplotypes && "ExcludeIsChimeric" == excMeta.first){
									chimeriaExcludedRescue = true;
									other = false;
								}
								if(pars.rescueExcludedOneOffLowFreqHaplotypes && "ExcludeFailedLowFreqOneOff" == excMeta.first){
									oneOffExcludedRescue = true;
									other = false;
								}
								if(other){
									otherExcludedCriteria.emplace(excMeta.first);
								}
							}
						}
						//check if it should be considered for rescue
						//std::cout << excluded.seqBase_.name_ << " consider for rescue: " << njh::colorBool((chimeriaExcludedRescue || oneOffExcludedRescue) && otherExcludedCriteria.empty()) << std::endl;
						if((chimeriaExcludedRescue || oneOffExcludedRescue) && otherExcludedCriteria.empty()){
							//see if it matches a major haplotype
							bool rescue = false;
							for(const auto & popHap : sampColl.popCollapse_->collapsed_.clusters_){
								if(popHap.seqBase_.seq_ == excluded.seqBase_.seq_ &&
										popHap.seqBase_.cnt_ > excluded.seqBase_.cnt_ &&
										njh::in(popHap.seqBase_.name_, majorHaps)){
									rescue = true;
									break;
								}
							}
							if(rescue){
								toBeRescued.emplace_back(excludedPos);
							}
						}
					}
				}
				if(!toBeRescued.empty()){
					rescuedHaplotypes = true;
					std::sort(toBeRescued.rbegin(), toBeRescued.rend());
					for(const auto toRescue : toBeRescued){
						MetaDataInName excludedMeta(sampPtr->excluded_.clusters_[toRescue].seqBase_.name_);
						excludedMeta.addMeta("rescue", "TRUE");
						excludedMeta.resetMetaInName(sampPtr->excluded_.clusters_[toRescue].seqBase_.name_);
						//unmarking so as not to mess up chimera numbers
						sampPtr->excluded_.clusters_[toRescue].seqBase_.unmarkAsChimeric();
						for (auto & subRead : sampPtr->excluded_.clusters_[toRescue].reads_) {
							subRead->seqBase_.unmarkAsChimeric();
						}
						sampPtr->collapsed_.clusters_.emplace_back(sampPtr->excluded_.clusters_[toRescue]);
						sampPtr->excluded_.clusters_.erase(sampPtr->excluded_.clusters_.begin() + toRescue);
					}
					sampPtr->updateAfterExclustion();
					sampPtr->renameClusters("fraction");
				}
				sampColl.dumpSample(sampleName);
			}
			if(rescuedHaplotypes){
				//if excluded run pop clustering again
				sampColl.doPopulationClustering(sampColl.createPopInput(),
						alignerObj, collapserObj, pars.popIteratorMap);
			}
		}// end resuce operations for chimeria and low freq haplotypes



		if(pars.removeCommonlyLowFreqHaplotypes_){
			while(sampColl.excludeCommonlyLowFreqHaps(pars.lowFreqHaplotypeFracCutOff_)){
				//if excluded run pop clustering again
				sampColl.doPopulationClustering(sampColl.createPopInput(),
						alignerObj, collapserObj, pars.popIteratorMap);
			}
		}

		if(pars.removeOneSampOnlyOneOffHaps){
			if(sampColl.excludeOneSampOnlyOneOffHaps(pars.oneSampOnlyOneOffHapsFrac, alignerObj)){
				//if excluded run pop clustering again
				sampColl.doPopulationClustering(sampColl.createPopInput(),
						alignerObj, collapserObj, pars.popIteratorMap);
			}
		}

		if(pars.removeOneSampOnlyHaps){
			if(sampColl.excludeOneSampOnlyHaps(pars.oneSampOnlyHapsFrac)){
				//if excluded run pop clustering again
				sampColl.doPopulationClustering(sampColl.createPopInput(),
						alignerObj, collapserObj, pars.popIteratorMap);
			}
		}

		if(pars.rescueMatchingExpected && !expectedSeqs.empty()){
			bool rescuedHaplotypes = false;
			for(const auto & sampleName : sampColl.passingSamples_){
				sampColl.setUpSampleFromPrevious(sampleName);
				auto sampPtr = sampColl.sampleCollapses_.at(sampleName);
				std::vector<uint32_t> toBeRescued;
				//iterator over haplotypes, determine if they should be considered for rescue, if they should be then check to see if they match a major haplotype
				for(const auto excludedPos : iter::range(sampPtr->excluded_.clusters_.size())){
					const auto & excluded = sampPtr->excluded_.clusters_[excludedPos];
					if(excluded.nameHasMetaData()){
						MetaDataInName excludedMeta(excluded.seqBase_.name_);
						std::set<std::string> otherExcludedCriteria;
						bool chimeriaExcludedRescue = false;
						bool oneOffExcludedRescue = false;
						bool commonlyLowExcludedRescue = false;
						for(const auto & excMeta : excludedMeta.meta_){
							if(njh::beginsWith(excMeta.first, "Exclude") ){
								bool other = true;
								if("ExcludeIsChimeric" == excMeta.first){
									chimeriaExcludedRescue = true;
									other = false;
								}
								if("ExcludeFailedLowFreqOneOff" == excMeta.first){
									oneOffExcludedRescue = true;
									other = false;
								}
								if("ExcludeFailedFracCutOff" == excMeta.first){
									commonlyLowExcludedRescue = true;
									other = false;
								}
								if(other){
									otherExcludedCriteria.emplace(excMeta.first);
								}
							}
						}
						//check if it should be considered for resuce
						if((chimeriaExcludedRescue || oneOffExcludedRescue || commonlyLowExcludedRescue) && otherExcludedCriteria.empty()){
							//see if it matches a major haplotype
							bool rescue = false;
							for(const auto & expectedHap : expectedSeqs){
								if(expectedHap.seqBase_.seq_ == excluded.seqBase_.seq_){
									rescue = true;
									break;
								}
							}
							if(rescue){
								toBeRescued.emplace_back(excludedPos);
							}
						}
					}
				}
				if(!toBeRescued.empty()){
					rescuedHaplotypes = true;
					std::sort(toBeRescued.rbegin(), toBeRescued.rend());
					for(const auto toRescue : toBeRescued){
						MetaDataInName excludedMeta(sampPtr->excluded_.clusters_[toRescue].seqBase_.name_);
						excludedMeta.addMeta("rescue", "TRUE");
						excludedMeta.resetMetaInName(sampPtr->excluded_.clusters_[toRescue].seqBase_.name_);
						//unmarking so as not to mess up chimera numbers
						sampPtr->excluded_.clusters_[toRescue].seqBase_.unmarkAsChimeric();
						for (auto & subRead : sampPtr->excluded_.clusters_[toRescue].reads_) {
							subRead->seqBase_.unmarkAsChimeric();
						}
						sampPtr->collapsed_.clusters_.emplace_back(sampPtr->excluded_.clusters_[toRescue]);
						sampPtr->excluded_.clusters_.erase(sampPtr->excluded_.clusters_.begin() + toRescue);
					}
					sampPtr->updateAfterExclustion();
					sampPtr->renameClusters("fraction");
				}
				sampColl.dumpSample(sampleName);
			}
			if(rescuedHaplotypes){
				//if excluded run pop clustering again
				sampColl.doPopulationClustering(sampColl.createPopInput(),
						alignerObj, collapserObj, pars.popIteratorMap);
			}
		} //end resue of matching expected
	}




	if(setUp.pars_.verbose_){
		std::cout << njh::bashCT::boldRed("Done Pop Clustering") << std::endl;
	}
	if ("" != pars.previousPopFilename && !pars.noPopulation) {
		sampColl.renamePopWithSeqs(getSeqs<readObject>(pars.previousPopFilename), pars.previousPopErrors);
	}

	if (!expectedSeqs.empty()) {
		sampColl.comparePopToRefSeqs(expectedSeqs, alignerObj);
	}

	sampColl.printSampleCollapseInfo(
			njh::files::make_path(sampColl.masterOutputDir_,
					"selectedClustersInfo.tab.txt"));

	if(pars.writeOutAllInfoFile){
		sampColl.printAllSubClusterInfo(
					njh::files::make_path(sampColl.masterOutputDir_,
							"allClustersInfo.tab.txt.gz"));
	}


	sampColl.symlinkInSampleFinals();
	sampColl.outputRepAgreementInfo();
	if(!pars.noPopulation){
		table hapIdTab = sampColl.genHapIdTable();
		hapIdTab.outPutContents(TableIOOpts::genTabFileOut(njh::files::make_path(sampColl.masterOutputDir_,
				"hapIdTable.tab.txt"), true));
		auto popSeqsPerSamp = sampColl.genOutPopSeqsPerSample();
		sampColl.dumpPopulation();
		SeqOutput::write(popSeqsPerSamp, SeqIOOptions::genFastqOut(njh::files::make_path(sampColl.masterOutputDir_, "population", "popSeqsWithMetaWtihSampleName")));
	}
	if("" != pars.groupingsFile){
		sampColl.createGroupInfoFiles();
	}

	sampColl.createCoreJsonFile();

	//collect extraction dirs
	std::set<bfs::path> extractionDirs;
	for(const auto & file : analysisFiles){
		auto fileToks = njh::tokenizeString(bfs::relative(file.first, pars.masterDir).string(), "/");
		if(njh::in(fileToks[0], pars.excludeSamples)){
			continue;
		}
		auto metaDataJsonFnp = njh::files::make_path(file.first.parent_path(), "metaData.json");
		if(bfs::exists(metaDataJsonFnp)){
			auto metaJson = njh::json::parseFile(metaDataJsonFnp.string());
			if(metaJson.isMember("extractionDir")){
				extractionDirs.emplace(metaJson["extractionDir"].asString());
			}
		}
	}
	if(setUp.pars_.verbose_){
		std::cout << "Extraction Dirs" << std::endl;
		std::cout << njh::conToStr(extractionDirs, "\n") << std::endl;
	}
	table profileTab;
	table statsTab;
	for(const auto & extractDir : extractionDirs){
		auto profileFnp = njh::files::make_path(extractDir, "extractionProfile.tab.txt");
		auto statsFnp = njh::files::make_path(extractDir, "extractionStats.tab.txt");
		if(bfs::exists(profileFnp)){
			table currentProfileTab(profileFnp.string(), "\t", true);
			auto oldColumnNames = currentProfileTab.columnNames_;
			currentProfileTab.addColumn(VecStr{extractDir.filename().string()}, "extractionDir");
			currentProfileTab = currentProfileTab.getColumns(concatVecs(VecStr{"extractionDir"}, oldColumnNames));
			if(profileTab.empty()){
				profileTab = currentProfileTab;
			}else{
				profileTab.rbind(currentProfileTab, false);
			}
		}
		if(bfs::exists(statsFnp)){
			table curentStatsTab(statsFnp.string(), "\t", true);
			auto oldColumnNames = curentStatsTab.columnNames_;
			curentStatsTab.addColumn(VecStr{extractDir.filename().string()}, "extractionDir");
			curentStatsTab = curentStatsTab.getColumns(concatVecs(VecStr{"extractionDir"}, oldColumnNames));
			if(statsTab.empty()){
				statsTab = curentStatsTab;
			}else{
				statsTab.rbind(curentStatsTab, false);
			}
		}
	}

	auto extractionOutputDir = njh::files::make_path(setUp.pars_.directoryName_,
			"extractionInfo");
	njh::files::makeDirP(njh::files::MkdirPar(extractionOutputDir.string()));
	if (!profileTab.empty()) {
		profileTab.sortTable("extractionDir", false);
		auto profileTabOpts =
				TableIOOpts::genTabFileOut(
						njh::files::make_path(extractionOutputDir,
								"extractionProfile.tab.txt").string(), true);
		profileTabOpts.out_.overWriteFile_ = true;
		profileTab.outPutContents(profileTabOpts);
	}
	if (!statsTab.empty()) {
		auto statsTabOpts =
				TableIOOpts::genTabFileOut(
						njh::files::make_path(extractionOutputDir,
								"extractionStats.tab.txt").string(), true);
		statsTabOpts.out_.overWriteFile_ = true;
		statsTab.sortTable("extractionDir", false);
		statsTab.outPutContents(statsTabOpts);
	}


	alignerObj.processAlnInfoOutput(setUp.pars_.outAlnInfoDirName_,
			setUp.pars_.verbose_);
	setUp.rLog_ << alignerObj.numberOfAlingmentsDone_ << "\n";
	if (setUp.pars_.verbose_) {
		std::cout << alignerObj.numberOfAlingmentsDone_ << std::endl;
		setUp.logRunTime(std::cout);
	}

	return 0;
}

}  // namespace njhseq
