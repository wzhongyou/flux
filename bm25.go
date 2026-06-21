package flux

import (
	"math"
	"strings"
	"sync"
	"unicode"
)

// ============================================================
// BM25 Index — text retrieval with BM25 scoring
// ============================================================

const (
	// DefaultBM25K1 controls term frequency saturation (1.2 is standard).
	DefaultBM25K1 = 1.2
	// DefaultBM25B controls length normalization (0.75 is standard).
	DefaultBM25B = 0.75
)

// BM25Index implements BM25Okapi ranking for full-text search.
// It tokenizes text into terms and maintains frequency statistics
// for BM25 scoring.
type BM25Index struct {
	mu         sync.RWMutex
	avgDocLen  float64
	totalDocs  int
	docLengths map[string]int            // docID -> total term count
	termFreq   map[string]map[string]int // term -> docID -> frequency
	docFreq    map[string]int            // term -> document count
	k1         float64
	b          float64
}

// NewBM25Index creates a new BM25 index with default parameters.
func NewBM25Index() *BM25Index {
	return &BM25Index{
		docLengths: make(map[string]int),
		termFreq:   make(map[string]map[string]int),
		docFreq:    make(map[string]int),
		k1:         DefaultBM25K1,
		b:          DefaultBM25B,
	}
}

// IndexDocument tokenizes the text and indexes each term for the docID.
// If the docID already exists, it is removed first (re-index).
func (idx *BM25Index) IndexDocument(docID string, text string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	// Remove old entry if exists
	if _, exists := idx.docLengths[docID]; exists {
		idx.removeDocument(docID)
	}

	terms := tokenize(text)
	if len(terms) == 0 {
		return
	}

	idx.docLengths[docID] = len(terms)

	// Count term frequencies within this document
	tf := make(map[string]int)
	for _, term := range terms {
		tf[term]++
	}

	for term, count := range tf {
		if idx.termFreq[term] == nil {
			idx.termFreq[term] = make(map[string]int)
		}
		idx.termFreq[term][docID] = count
		idx.docFreq[term]++
	}

	idx.totalDocs++
	idx.updateAvgDocLen()
}

// RemoveDocument removes a document from the index.
func (idx *BM25Index) RemoveDocument(docID string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.removeDocument(docID)
}

func (idx *BM25Index) removeDocument(docID string) {
	_, exists := idx.docLengths[docID]
	if !exists {
		return
	}

	// Remove term frequencies for this document
	for term, docMap := range idx.termFreq {
		if _, ok := docMap[docID]; ok {
			delete(idx.termFreq[term], docID)
			idx.docFreq[term]--
			if idx.docFreq[term] <= 0 {
				delete(idx.docFreq, term)
				delete(idx.termFreq, term)
			}
		}
	}

	delete(idx.docLengths, docID)
	idx.totalDocs--

	// Recalculate average doc length
	if idx.totalDocs > 0 {
		var totalLen int
		for _, l := range idx.docLengths {
			totalLen += l
		}
		idx.avgDocLen = float64(totalLen) / float64(idx.totalDocs)
	} else {
		idx.avgDocLen = 0
	}
}

// Score calculates BM25 scores for all documents that match any term in the query.
// Returns a map of docID -> BM25 score.
func (idx *BM25Index) Score(query string) map[string]float64 {
	idx.mu.RLock()
	defer idx.mu.RUnlock()

	if idx.totalDocs == 0 {
		return nil
	}

	queryTerms := tokenize(query)
	if len(queryTerms) == 0 {
		return nil
	}

	// Deduplicate query terms
	seen := make(map[string]bool)
	var uniqueTerms []string
	for _, t := range queryTerms {
		if !seen[t] {
			seen[t] = true
			uniqueTerms = append(uniqueTerms, t)
		}
	}

	N := float64(idx.totalDocs)
	scores := make(map[string]float64)

	for _, term := range uniqueTerms {
		df := idx.docFreq[term]
		if df == 0 {
			continue
		}

		// IDF: ln(1 + (N - df + 0.5) / (df + 0.5))
		idf := math.Log(1 + (N-float64(df)+0.5)/(float64(df)+0.5))

		docMap := idx.termFreq[term]
		for docID, tf := range docMap {
			docLen := float64(idx.docLengths[docID])
			// BM25 score for this term
			// (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * docLen / avgDocLen))
			denom := float64(tf) + idx.k1*(1-idx.b+idx.b*docLen/idx.avgDocLen)
			termScore := idf * (float64(tf) * (idx.k1 + 1) / denom)
			scores[docID] += termScore
		}
	}

	return scores
}

// Len returns the number of indexed documents.
func (idx *BM25Index) Len() int {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return idx.totalDocs
}

// Clear removes all documents from the index.
func (idx *BM25Index) Clear() {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.docLengths = make(map[string]int)
	idx.termFreq = make(map[string]map[string]int)
	idx.docFreq = make(map[string]int)
	idx.totalDocs = 0
	idx.avgDocLen = 0
}

func (idx *BM25Index) updateAvgDocLen() {
	var totalLen int
	for _, l := range idx.docLengths {
		totalLen += l
	}
	idx.avgDocLen = float64(totalLen) / float64(idx.totalDocs)
}

// tokenize splits text into lowercase terms, removing punctuation.
func tokenize(text string) []string {
	var terms []string
	var current strings.Builder

	for _, r := range text {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			current.WriteRune(unicode.ToLower(r))
		} else {
			if current.Len() > 0 {
				terms = append(terms, current.String())
				current.Reset()
			}
		}
	}
	if current.Len() > 0 {
		terms = append(terms, current.String())
	}
	return terms
}
