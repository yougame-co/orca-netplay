
#include "VideoBackends/Metal/MTLShader.h"

#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLUtil.h"
#include "VideoCommon/ShaderCacheData.h"

Metal::Shader::Shader(ShaderStage stage, std::string msl, MRCOwned<id<MTLFunction>> shader)
    : AbstractShader(stage), m_msl(std::move(msl)), m_shader(std::move(shader))
{
}

Metal::Shader::~Shader()
{
  g_object_cache->ShaderDestroyed(this);
}

AbstractShader::BinaryData Metal::Shader::GetBinary() const
{
  return VideoCommon::ShaderCacheData::Seal(
      Util::ShaderCacheIdentity(GetStage()),
      {reinterpret_cast<const u8*>(m_msl.data()), m_msl.size()});
}
