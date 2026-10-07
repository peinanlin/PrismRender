"""CPU-only checks for rendering-path measurement statistics."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("rendering_path_summary", Path(__file__).resolve().parents[1] / "tools/SummarizeRenderingPaths.py")
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)

def observation(observed_frame, source_frame, duration):
    return {"frameId": observed_frame, "views": {"game": {"gpuResolvedFrameId": source_frame, "gpuTotalMs": duration}}}

class RenderingPathSummaryTests(unittest.TestCase):
    def test_even_median_and_nearest_rank_p95(self):
        result = summary.distribution(list(range(1, 21)))
        self.assertEqual(result["median"], 10.5)
        self.assertEqual(result["p95"], 19)
        self.assertEqual(result["mean"], 10.5)

    def test_gpu_drain_is_associated_with_source_frame(self):
        # Sources 3..4 return during drain observations 5..6.
        frames = [observation(1, None, None), observation(3, 1, 9.), observation(4, 2, 8.), observation(5, 3, 1.25), observation(6, 4, 2.5)]
        self.assertEqual(summary.source_gpu_samples(frames, 3, 4), {3: 1.25, 4: 2.5})

    def test_missing_or_duplicate_gpu_samples_are_rejected(self):
        sample = observation(5, 3, 1.25)
        with self.assertRaisesRegex(ValueError, "Missing completed GPU"):
            summary.source_gpu_samples([sample], 3, 4)
        with self.assertRaisesRegex(ValueError, "Duplicate GPU"):
            summary.source_gpu_samples([sample, sample], 3, 3)

    def test_present_interval_is_optional_for_older_profiler(self):
        self.assertIsNone(summary.present_interval({"pipeline": {}}, {"pipeline": {}}))
        def frame(value):
            return {"pipeline": {"steadyTimelineNanoseconds": {"timingContractVersion": 2, "presentReturned": value}}}
        self.assertEqual(summary.present_interval(frame(5_000_000), frame(2_000_000)), 3.0)
        with self.assertRaises(ValueError):
            summary.present_interval(frame(2_000_000), frame(5_000_000))

    def test_invalid_timings_are_rejected(self):
        for values in ([], [None], [0], [-1], [float("nan")], [float("inf")]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                summary.distribution(values)

if __name__ == "__main__":
    unittest.main()
