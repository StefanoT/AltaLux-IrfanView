#include "../AltaLux/Segmentation/SegmentationApi.h"

#include <Windows.h>
#include <bcrypt.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
	constexpr int kModelSize = 1024;
	constexpr const wchar_t* kEncoderFile = L"AltaLuxMobileSAMEncoder.onnx";
	constexpr const wchar_t* kDecoderFile = L"AltaLuxMobileSAMDecoder.onnx";
	constexpr const wchar_t* kManifestFile = L"AltaLuxSegmentation.models.json";
	constexpr float kMean[3] = { 123.675f, 116.28f, 103.53f };
	constexpr float kStd[3] = { 58.395f, 57.12f, 57.375f };

	std::wstring GetModuleDirectory()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GetModuleDirectory), &module);
		wchar_t path[32768] = {};
		const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(_countof(path)));
		if (length == 0 || length >= _countof(path)) return {};
		wchar_t* separator = wcsrchr(path, L'\\');
		if (separator == nullptr) return {};
		*(separator + 1) = L'\0';
		return path;
	}

	std::wstring JoinPath(const std::wstring& directory, const wchar_t* filename)
	{
		return directory + filename;
	}

	std::string ReadTextFile(const std::wstring& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) throw std::runtime_error("The AltaLux model manifest is missing");
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	std::string ExtractJsonString(const std::string& json, const char* section, const char* key)
	{
		std::size_t position = json.find(std::string("\"") + section + "\"");
		if (position == std::string::npos) throw std::runtime_error("Invalid AltaLux model manifest section");
		position = json.find(std::string("\"") + key + "\"", position);
		if (position == std::string::npos) throw std::runtime_error("Invalid AltaLux model manifest value");
		position = json.find(':', position);
		position = json.find('"', position);
		if (position == std::string::npos) throw std::runtime_error("Invalid AltaLux model manifest string");
		const std::size_t end = json.find('"', position + 1);
		if (end == std::string::npos) throw std::runtime_error("Invalid AltaLux model manifest string");
		return json.substr(position + 1, end - position - 1);
	}

	std::string Sha256File(const std::wstring& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) throw std::runtime_error("An AltaLux segmentation model is missing");
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		DWORD objectLength = 0;
		DWORD hashLength = 0;
		DWORD received = 0;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
			BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength), &received, 0) < 0 ||
			BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
				sizeof(hashLength), &received, 0) < 0)
		{
			if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
			throw std::runtime_error("Unable to initialize SHA-256 validation");
		}
		std::vector<unsigned char> object(objectLength);
		std::vector<unsigned char> digest(hashLength);
		if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0)
		{
			BCryptCloseAlgorithmProvider(algorithm, 0);
			throw std::runtime_error("Unable to create SHA-256 validation state");
		}
		std::vector<unsigned char> buffer(1024U * 1024U);
		while (stream)
		{
			stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
			const std::streamsize count = stream.gcount();
			if (count > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
			{
				BCryptDestroyHash(hash);
				BCryptCloseAlgorithmProvider(algorithm, 0);
				throw std::runtime_error("Unable to hash an AltaLux model");
			}
		}
		if (BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0)
		{
			BCryptDestroyHash(hash);
			BCryptCloseAlgorithmProvider(algorithm, 0);
			throw std::runtime_error("Unable to finish AltaLux model validation");
		}
		BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		std::ostringstream output;
		output << std::hex << std::setfill('0');
		for (unsigned char value : digest) output << std::setw(2) << static_cast<unsigned int>(value);
		return output.str();
	}

	void ValidateManifest(const std::wstring& directory)
	{
		const std::string manifest = ReadTextFile(JoinPath(directory, kManifestFile));
		const std::string encoderFile = ExtractJsonString(manifest, "encoder", "file");
		const std::string decoderFile = ExtractJsonString(manifest, "decoder", "file");
		if (encoderFile != "AltaLuxMobileSAMEncoder.onnx" || decoderFile != "AltaLuxMobileSAMDecoder.onnx")
		{
			throw std::runtime_error("The AltaLux model manifest contains unexpected filenames");
		}
		const std::string encoderHash = ExtractJsonString(manifest, "encoder", "sha256");
		const std::string decoderHash = ExtractJsonString(manifest, "decoder", "sha256");
		if (encoderHash.size() != 64 || decoderHash.size() != 64 ||
			Sha256File(JoinPath(directory, kEncoderFile)) != encoderHash ||
			Sha256File(JoinPath(directory, kDecoderFile)) != decoderHash)
		{
			throw std::runtime_error("AltaLux segmentation model hash validation failed");
		}
	}

	class MobileSamEngine final : public ISegmentationEngine
	{
	public:
		MobileSamEngine()
			: environment_(ORT_LOGGING_LEVEL_WARNING, "AltaLuxSegmentation"),
			memoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
		{
			sessionOptions_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
			sessionOptions_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
			sessionOptions_.SetIntraOpNumThreads(0);
			const std::wstring directory = GetModuleDirectory();
			if (directory.empty()) throw std::runtime_error("Unable to locate AltaLuxSegmentation.dll");
			ValidateManifest(directory);
			encoder_.reset(new Ort::Session(environment_, JoinPath(directory, kEncoderFile).c_str(), sessionOptions_));
			decoder_.reset(new Ort::Session(environment_, JoinPath(directory, kDecoderFile).c_str(), sessionOptions_));
		}

		SegmentationStatus PrepareImage(const SegmentationImageView& image) noexcept override
		{
			try
			{
				if (!ValidateImage(image)) return Fail(SegmentationStatus::InvalidArgument, L"Invalid source image.");
				ResetCancellation();
				width_ = static_cast<int>(image.width);
				height_ = static_cast<int>(image.height);
				scale_ = static_cast<float>(kModelSize) / static_cast<float>((std::max)(width_, height_));
				resizedWidth_ = (std::max)(1, static_cast<int>(width_ * scale_ + 0.5f));
				resizedHeight_ = (std::max)(1, static_cast<int>(height_ * scale_ + 0.5f));
				std::vector<float> input(static_cast<std::size_t>(3) * kModelSize * kModelSize, 0.0f);
				FillEncoderInput(image, input);
				if (cancelled_) return Fail(SegmentationStatus::Cancelled, L"Image analysis was cancelled.");

				const std::array<int64_t, 4> shape = { 1, 3, kModelSize, kModelSize };
				Ort::Value tensor = Ort::Value::CreateTensor<float>(memoryInfo_, input.data(), input.size(), shape.data(), shape.size());
				const char* inputNames[] = { "input_image" };
				const char* outputNames[] = { "image_embeddings" };
				auto output = encoder_->Run(runOptions_, inputNames, &tensor, 1, outputNames, 1);
				const auto outputShape = output[0].GetTensorTypeAndShapeInfo().GetShape();
				const std::size_t elementCount = output[0].GetTensorTypeAndShapeInfo().GetElementCount();
				imageEmbeddingShape_ = outputShape;
				const float* embedding = output[0].GetTensorData<float>();
				imageEmbedding_.assign(embedding, embedding + elementCount);
				prepared_ = true;
				lastError_.clear();
				return SegmentationStatus::Ok;
			}
			catch (const Ort::Exception& error)
			{
				return Fail(cancelled_ ? SegmentationStatus::Cancelled : SegmentationStatus::ModelError,
					Widen(error.what()));
			}
			catch (const std::exception& error)
			{
				return Fail(SegmentationStatus::RuntimeError, Widen(error.what()));
			}
		}

		SegmentationStatus SegmentPoint(const SegmentationPoint& point, SegmentationMaskView& mask) noexcept override
		{
			try
			{
				if (!prepared_) return Fail(SegmentationStatus::NotReady, L"Analyze the image before selecting an object.");
				if (!ValidateMask(mask) || point.x < 0.0f || point.y < 0.0f || point.x >= width_ || point.y >= height_)
				{
					return Fail(SegmentationStatus::InvalidArgument, L"Invalid point or output mask.");
				}
				ResetCancellation();
				std::vector<float> pointCoords = {
					(point.x + 0.5f) * resizedWidth_ / width_,
					(point.y + 0.5f) * resizedHeight_ / height_
				};
				std::vector<float> pointLabels = { 1.0f };
				std::vector<float> maskInput(256U * 256U, 0.0f);
				std::vector<float> hasMaskInput = { 0.0f };
				std::vector<float> originalSize = { static_cast<float>(height_), static_cast<float>(width_) };

				const std::array<int64_t, 3> pointShape = { 1, 1, 2 };
				const std::array<int64_t, 2> labelShape = { 1, 1 };
				const std::array<int64_t, 4> maskShape = { 1, 1, 256, 256 };
				const std::array<int64_t, 1> scalarShape = { 1 };
				const std::array<int64_t, 1> sizeShape = { 2 };
				std::vector<Ort::Value> inputs;
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, imageEmbedding_.data(), imageEmbedding_.size(),
					imageEmbeddingShape_.data(), imageEmbeddingShape_.size()));
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, pointCoords.data(), pointCoords.size(), pointShape.data(), pointShape.size()));
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, pointLabels.data(), pointLabels.size(), labelShape.data(), labelShape.size()));
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, maskInput.data(), maskInput.size(), maskShape.data(), maskShape.size()));
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, hasMaskInput.data(), hasMaskInput.size(), scalarShape.data(), scalarShape.size()));
				inputs.emplace_back(Ort::Value::CreateTensor<float>(memoryInfo_, originalSize.data(), originalSize.size(), sizeShape.data(), sizeShape.size()));
				const char* inputNames[] = { "image_embeddings", "point_coords", "point_labels", "mask_input", "has_mask_input", "orig_im_size" };
				const char* outputNames[] = { "masks", "iou_predictions", "low_res_masks" };
				auto outputs = decoder_->Run(runOptions_, inputNames, inputs.data(), inputs.size(), outputNames, 3);
				if (cancelled_) return Fail(SegmentationStatus::Cancelled, L"Object selection was cancelled.");

				const std::vector<int64_t> maskOutputShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
				if (maskOutputShape.size() != 4 || maskOutputShape[2] != height_ || maskOutputShape[3] != width_)
				{
					return Fail(SegmentationStatus::ModelError, L"The decoder returned an unexpected mask shape.");
				}
				const int candidateCount = static_cast<int>(maskOutputShape[1]);
				const float* scores = outputs[1].GetTensorData<float>();
				int bestCandidate = 0;
				for (int candidate = 1; candidate < candidateCount; ++candidate)
				{
					if (scores[candidate] > scores[bestCandidate]) bestCandidate = candidate;
				}
				const std::size_t candidatePixels = static_cast<std::size_t>(width_) * height_;
				const float* masks = outputs[0].GetTensorData<float>() + bestCandidate * candidatePixels;
				for (int y = 0; y < height_; ++y)
				{
					std::uint8_t* row = mask.pixels + static_cast<std::ptrdiff_t>(y) * mask.stride;
					for (int x = 0; x < width_; ++x)
					{
						row[x] = masks[static_cast<std::size_t>(y) * width_ + x] > 0.0f ? 255 : 0;
					}
				}
				lastError_.clear();
				return SegmentationStatus::Ok;
			}
			catch (const Ort::Exception& error)
			{
				return Fail(cancelled_ ? SegmentationStatus::Cancelled : SegmentationStatus::ModelError,
					Widen(error.what()));
			}
			catch (const std::exception& error)
			{
				return Fail(SegmentationStatus::RuntimeError, Widen(error.what()));
			}
		}

		void Cancel() noexcept override
		{
			cancelled_ = true;
			try { runOptions_.SetTerminate(); } catch (...) {}
		}

		const wchar_t* GetLastError() const noexcept override
		{
			return lastError_.c_str();
		}

		void Release() noexcept override
		{
			delete this;
		}

	private:
		static std::wstring Widen(const char* text)
		{
			if (text == nullptr) return L"Unknown inference error.";
			const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
			if (length <= 1) return L"Unknown inference error.";
			std::wstring result(static_cast<std::size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], length);
			result.pop_back();
			return result;
		}

		SegmentationStatus Fail(SegmentationStatus status, const std::wstring& error)
		{
			lastError_ = error;
			return status;
		}

		void ResetCancellation()
		{
			cancelled_ = false;
			runOptions_.UnsetTerminate();
		}

		bool ValidateImage(const SegmentationImageView& image) const
		{
			const int pixelSize = image.format == SegmentationPixelFormat::Bgr24 ? 3 :
				(image.format == SegmentationPixelFormat::Bgra32 ? 4 : 0);
			return image.pixels != nullptr && image.width > 0 && image.height > 0 && pixelSize != 0 &&
				std::abs(image.stride) >= static_cast<int>(image.width) * pixelSize;
		}

		bool ValidateMask(const SegmentationMaskView& mask) const
		{
			return mask.pixels != nullptr && mask.width == static_cast<std::uint32_t>(width_) &&
				mask.height == static_cast<std::uint32_t>(height_) && std::abs(mask.stride) >= width_;
		}

		void FillEncoderInput(const SegmentationImageView& image, std::vector<float>& input)
		{
			const int pixelSize = image.format == SegmentationPixelFormat::Bgr24 ? 3 : 4;
			const std::size_t planeSize = static_cast<std::size_t>(kModelSize) * kModelSize;
			for (int y = 0; y < resizedHeight_; ++y)
			{
				if (cancelled_) return;
				const float sourceY = (y + 0.5f) * height_ / resizedHeight_ - 0.5f;
				const int y0 = (std::max)(0, (std::min)(height_ - 1, static_cast<int>(std::floor(sourceY))));
				const int y1 = (std::min)(height_ - 1, y0 + 1);
				const float fy = (std::max)(0.0f, sourceY - std::floor(sourceY));
				const std::uint8_t* row0 = image.pixels + static_cast<std::ptrdiff_t>(y0) * image.stride;
				const std::uint8_t* row1 = image.pixels + static_cast<std::ptrdiff_t>(y1) * image.stride;
				for (int x = 0; x < resizedWidth_; ++x)
				{
					const float sourceX = (x + 0.5f) * width_ / resizedWidth_ - 0.5f;
					const int x0 = (std::max)(0, (std::min)(width_ - 1, static_cast<int>(std::floor(sourceX))));
					const int x1 = (std::min)(width_ - 1, x0 + 1);
					const float fx = (std::max)(0.0f, sourceX - std::floor(sourceX));
					for (int rgbChannel = 0; rgbChannel < 3; ++rgbChannel)
					{
						const int bgrChannel = 2 - rgbChannel;
						const float top = row0[x0 * pixelSize + bgrChannel] * (1.0f - fx) + row0[x1 * pixelSize + bgrChannel] * fx;
						const float bottom = row1[x0 * pixelSize + bgrChannel] * (1.0f - fx) + row1[x1 * pixelSize + bgrChannel] * fx;
						const float value = top * (1.0f - fy) + bottom * fy;
						input[static_cast<std::size_t>(rgbChannel) * planeSize + y * kModelSize + x] =
							(value - kMean[rgbChannel]) / kStd[rgbChannel];
					}
				}
			}
		}

		Ort::Env environment_;
		Ort::SessionOptions sessionOptions_;
		Ort::RunOptions runOptions_;
		Ort::MemoryInfo memoryInfo_;
		std::unique_ptr<Ort::Session> encoder_;
		std::unique_ptr<Ort::Session> decoder_;
		std::vector<float> imageEmbedding_;
		std::vector<int64_t> imageEmbeddingShape_;
		std::atomic<bool> cancelled_{ false };
		std::wstring lastError_;
		int width_ = 0;
		int height_ = 0;
		int resizedWidth_ = 0;
		int resizedHeight_ = 0;
		float scale_ = 1.0f;
		bool prepared_ = false;
	};
}

extern "C" __declspec(dllexport) SegmentationStatus AltaLuxCreateSegmentationEngine(
	std::uint32_t abiVersion, ISegmentationEngine** engine) noexcept
{
	if (engine == nullptr) return SegmentationStatus::InvalidArgument;
	*engine = nullptr;
	if (abiVersion != ALTALUX_SEGMENTATION_ABI_VERSION) return SegmentationStatus::UnsupportedAbi;
	try
	{
		*engine = new MobileSamEngine();
		return SegmentationStatus::Ok;
	}
	catch (...)
	{
		return SegmentationStatus::RuntimeError;
	}
}
