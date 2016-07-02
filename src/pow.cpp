// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "primitives/block.h"
#include "uint256.h"
#include "util.h"

unsigned int GetNextWorkRequiredV2(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    int64_t retargetBlockCountInterval;
    int64_t lookupBlockCount;
    if (pindexLast->nHeight > 192237) {
        // after block 192240, switch to 540 retarget-window
        // with 1.25 limits
        retargetBlockCountInterval = 540; // retarget every 540 blocks
        lookupBlockCount = 540; // past blocks to use for timing
    } else {
        retargetBlockCountInterval = 2160; // retarget every 2160 blocks
        lookupBlockCount = 2160; // past blocks to use for timing
    }

    const int64_t retargetTimespan = 120 * retargetBlockCountInterval; // 2 minutes per block
    const int64_t retargetVsInspectRatio = lookupBlockCount / retargetBlockCountInterval; // currently 12

    // non-retargetting block: keep same diff:
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0 || (pindexLast->nHeight < lookupBlockCount)) {
        return (pindexLast->nBits);
    }

    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(pindexLast->nHeight - lookupBlockCount);
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    nActualTimespan = nActualTimespan / retargetVsInspectRatio;

    if (pindexLast->nHeight > 192237) {
        // at and after block 192240, use 1.25 limits:
        if (nActualTimespan < retargetTimespan / 1.25) {
            nActualTimespan = retargetTimespan / 1.25;
        }
        if (nActualTimespan > retargetTimespan * 1.25) {
            nActualTimespan = retargetTimespan * 1.25;
        }
    } else {
        if (nActualTimespan < retargetTimespan / 4) {
            nActualTimespan = retargetTimespan / 4;
        }
        if (nActualTimespan > retargetTimespan * 4) {
            nActualTimespan = retargetTimespan * 4;
        }
    }

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= retargetTimespan;

    // during the switchover from EMA retargetting to static 2160 retargetting:
    // temporary, low diff limit: 17.4k self deactivating at height 182000
    // (for first retarget only)
    if (pindexLast->nHeight < 183000) {
        arith_uint256 seventeenThousandsLimit;
        seventeenThousandsLimit.SetCompact(0x1b03bf8b);
        if (bnNew > seventeenThousandsLimit) {
            bnNew = seventeenThousandsLimit;
        }
    }

    // temporary, super ugly way to never, ever return diff < 5254,
    // just in the case something really bad happens
    // self-deactivate at block 220000
    if (pindexLast->nHeight < 220000) {
        arith_uint256 fiveThousandsLimit;
        fiveThousandsLimit.SetCompact(0x1b0c7898);
        if (bnNew > fiveThousandsLimit) {
            bnNew = fiveThousandsLimit;
        }
    }

    if (bnNew > nProofOfWorkLimit)
        bnNew = nProofOfWorkLimit;

    return bnNew.GetCompact();
}
unsigned int GetNextWorkRequiredEMA(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // ugly hack for some 32-bits machines @ block 137162
    if (pindexLast->nHeight == 137161) {
        return (0x1b034c51);
    }

    int64_t block_durations[2160];
    float alpha = 0.09; // closer to 1.0 = faster response to new values
    if (pindexLast->nHeight > 110322) {
        alpha = 0.06;
    }
    float accumulator = 120;
    const int64_t perBlockTargetTimespan = 120; // two mins between blocks

    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    if (pblock->nTime > pindexLast->nTime + perBlockTargetTimespan*10) {
	if (pindexLast->nHeight < 175000)
	{
		arith_uint256 bnNew;
		bnNew.SetCompact(pindexLast->nBits);

		if (pindexLast->nHeight> 101631 && pindexLast->nHeight < 103791) {
			bnNew *= 10;
		} else {
			bnNew *= 2;
		}

		// super ugly way to never, ever return diff < 5254:
		if (pindexLast->nHeight > 104290) {
			arith_uint256 fiveThousandsLimit;
			fiveThousandsLimit.SetCompact(0x1b0c7898);
			if (bnNew > fiveThousandsLimit) {
				bnNew = fiveThousandsLimit;
			}
		}

		if (bnNew > nProofOfWorkLimit)
		    bnNew = nProofOfWorkLimit;
		return bnNew.GetCompact();
	}
    }

    // collect last 3 days (30*24*3=2160) blocks durations:
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < 2160 ; i++) {
        block_durations[2159 - i] = pindexFirst->GetBlockTime() - pindexFirst->pprev->GetBlockTime();

        if (pindexLast->nHeight > 110322) {
            // slow down difficulty decrease even more,
            // also limit the effect of future nTime values (actually annihilates them):
            if (block_durations[2159 - i] > (1.5 * perBlockTargetTimespan) ) {
                block_durations[2159 - i] = 1.5 * perBlockTargetTimespan;
            }

            // slow down difficulty increase:
            if ((block_durations[2159 - i] >= 0) && (block_durations[2159 - i] < (perBlockTargetTimespan / 2)) ) {
                block_durations[2159 - i] = perBlockTargetTimespan / 2;
            }
        }

        if (block_durations[2159 - i] < 0 && pindexLast->nHeight > 104290) {
            block_durations[2159 - i] = perBlockTargetTimespan;
        }
        pindexFirst = pindexFirst->pprev;
    }

    // compute exponential moving average block duration:
    for (int i=0; i<2160 ; i++) {
        accumulator = (alpha * block_durations[i]) + (1 - alpha) * accumulator;

    }
    int64_t nActualTimespan = accumulator;

    if (nActualTimespan < perBlockTargetTimespan / 2)
        nActualTimespan = perBlockTargetTimespan / 2;

    if (pindexLast->nHeight > 110322) {
	// symetrical adjustments, both sides:
        if (nActualTimespan > perBlockTargetTimespan * 2) {
            nActualTimespan = perBlockTargetTimespan * 2;
        }
    } else {
        if (nActualTimespan > perBlockTargetTimespan * 4) {
            nActualTimespan = perBlockTargetTimespan * 4;
        }
    }

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= perBlockTargetTimespan;

    // temporary, super ugly way to never, ever return diff < 5254:
    if (pindexLast->nHeight > 104290) {
        arith_uint256 fiveThousandsLimit;
        fiveThousandsLimit.SetCompact(0x1b0c7898);
        if (bnNew > fiveThousandsLimit) {
            bnNew = fiveThousandsLimit;
        }
    }

    if (bnNew > nProofOfWorkLimit)
        bnNew = nProofOfWorkLimit;

    return bnNew.GetCompact();

}

unsigned int GetNextWorkRequiredV1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 1 hourworth of blocks
    int nBlocksLookupRange = params.DifficultyAdjustmentInterval()-1;
    if (pindexLast->nHeight > 99988) {
	nBlocksLookupRange = params.DifficultyAdjustmentInterval()  * 24;
    }
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor( pindexLast->nHeight - nBlocksLookupRange);
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    if (pindexLast->nHeight > 101908) {
        nActualTimespan = nActualTimespan / 3;
    } else if (pindexLast->nHeight > 99988) {
        nActualTimespan = nActualTimespan / 24;
    }
    LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    /// debug print
    LogPrintf("GetNextWorkRequired RETARGET\n");
    LogPrintf("params.nPowTargetTimespan = %d    nActualTimespan = %d\n", params.nPowTargetTimespan, nActualTimespan);
    LogPrintf("Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
    LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    if (pindexLast->nHeight > 101631) {
	return GetNextWorkRequiredEMA(pindexLast, pblock, params);
    } 
    //else if (pindexLast->nHeight > 181200) {
        //return GetBasicNextWorkRequired(pindexLast, pblock, params);
    //}
    return GetNextWorkRequiredV1(pindexLast, pblock, params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (pindexLast->nHeight > 101908) {
        nActualTimespan = nActualTimespan / 3;
    } else if (pindexLast->nHeight > 99988) {
        nActualTimespan = nActualTimespan / 24;
    }
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
