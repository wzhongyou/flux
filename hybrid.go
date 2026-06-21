package flux

import (
	"math"
	"sort"
)

// ============================================================
// Hybrid Search — combines vector similarity + BM25 text search
// ============================================================

// HybridSearchResult represents a single result from a hybrid search.
type HybridSearchResult struct {
	ID            string     `json:"id"`
	VectorScore   float64    `json:"vector_score"`
	TextScore     float64    `json:"text_score"`
	CombinedScore float64    `json:"combined_score"`
	Document      *Document  `json:"document,omitempty"`
}

// HybridSearch performs a combined vector + text search.
// Parameters:
//   - query: vector query for similarity search
//   - textQuery: text query for BM25 search
//   - k: number of results to return
//   - alpha: weight for vector score (0 = pure text, 1 = pure vector)
//   - filter: optional metadata filter
//
// If alpha is negative, Reciprocal Rank Fusion (RRF) is used instead
// of weighted sum.
func (c *Collection) HybridSearch(query []float64, textQuery string, k int, alpha float64, filter FilterFunc) []HybridSearchResult {
	if c.BM25 == nil || textQuery == "" {
		// Fall back to vector-only search
		vectorResults := c.Search(query, k, filter)
		results := make([]HybridSearchResult, len(vectorResults))
		for i, r := range vectorResults {
			results[i] = HybridSearchResult{
				ID:            r.ID,
				VectorScore:   r.Score,
				TextScore:     0,
				CombinedScore: r.Score,
				Document:      r.Document,
			}
		}
		return results
	}

	// Get BM25 scores
	textScores := c.BM25.Score(textQuery)

	// Determine search pool: candidates from both vector and text sides
	candidateSet := make(map[string]bool)
	for id := range textScores {
		candidateSet[id] = true
	}

	// Vector search — use brute force since we need all docs for hybrid ranking
	var vectorResults []SearchResult
	if c.Index != nil {
		vectorResults, _ = c.Index.SearchInternal(query, len(c.Docs))
	} else {
		vectorResults = c.bruteForceSearch(query, len(c.Docs), filter)
	}
	for _, r := range vectorResults {
		candidateSet[r.ID] = true
	}

	// If no candidates, return empty
	if len(candidateSet) == 0 {
		return nil
	}

	// Build result set
	results := make([]HybridSearchResult, 0, len(candidateSet))

	// Normalize vector scores to [0, 1]
	var maxVecScore float64
	var minVecScore float64 = math.MaxFloat64
	vectorScoreMap := make(map[string]float64)
	for _, r := range vectorResults {
		if candidateSet[r.ID] {
			score := max(0, r.Score) // clamp negatives
			vectorScoreMap[r.ID] = score
			if score > maxVecScore {
				maxVecScore = score
			}
			if score < minVecScore {
				minVecScore = score
			}
		}
	}
	vecRange := maxVecScore - minVecScore
	if vecRange == 0 {
		vecRange = 1
	}

	// Normalize text scores to [0, 1]
	var maxTextScore float64
	var minTextScore float64 = math.MaxFloat64
	for id, score := range textScores {
		if candidateSet[id] {
			if score > maxTextScore {
				maxTextScore = score
			}
			if score < minTextScore {
				minTextScore = score
			}
		}
	}
	textRange := maxTextScore - minTextScore
	if textRange == 0 {
		textRange = 1
	}

	// Build ranked lists for RRF
	vectorRanked := make([]string, 0, len(vectorResults))
	for _, r := range vectorResults {
		if candidateSet[r.ID] {
			vectorRanked = append(vectorRanked, r.ID)
		}
	}
	textRanked := make([]string, 0, len(textScores))
	for id := range textScores {
		if candidateSet[id] {
			textRanked = append(textRanked, id)
		}
	}
	// Rank text results by score descending
	sort.Slice(textRanked, func(i, j int) bool {
		return textScores[textRanked[i]] > textScores[textRanked[j]]
	})

	vectorRank := make(map[string]int)
	for i, id := range vectorRanked {
		vectorRank[id] = i + 1
	}
	textRank := make(map[string]int)
	for i, id := range textRanked {
		textRank[id] = i + 1
	}

	// Compute combined scores
	for id := range candidateSet {
		// Get document reference
		doc := c.Docs[id]

		// Skip if filter rejects this document
		if filter != nil && !filter(doc.Metadata) {
			continue
		}

		vecNorm := (vectorScoreMap[id] - minVecScore) / vecRange
		textNorm := (textScores[id] - minTextScore) / textRange

		var combined float64
		if alpha < 0 {
			// RRF
			const rrfK = 60.0
			vRank := vectorRank[id]
			tRank := textRank[id]
			combined = 0
			if vRank > 0 {
				combined += 1.0 / (rrfK + float64(vRank))
			}
			if tRank > 0 {
				combined += 1.0 / (rrfK + float64(tRank))
			}
		} else {
			// Weighted sum
			combined = alpha*vecNorm + (1-alpha)*textNorm
		}

		results = append(results, HybridSearchResult{
			ID:            id,
			VectorScore:   vecNorm,
			TextScore:     textNorm,
			CombinedScore: combined,
			Document:      doc,
		})
	}

	// Sort by combined score descending
	sort.Slice(results, func(i, j int) bool {
		return results[i].CombinedScore > results[j].CombinedScore
	})

	if len(results) > k {
		results = results[:k]
	}

	return results
}
