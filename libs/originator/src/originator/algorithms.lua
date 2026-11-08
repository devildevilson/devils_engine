-- Механика небольших управляющих графов/кривых принадлежит библиотеке, а правила выбора
-- переходов и профилей — проекту. Все таблицы живут в вызывающем Lua host; нет TLS,
-- фоновых задач и скрытого кэша. Индексы adjacency могут быть полными Lua integer keys.
local A={}
local axes={'x','y','z'}
function A.adjacency(nodes,edge_count,endpoints)
  assert(math.tointeger(edge_count) and edge_count>=0,'adjacency: invalid edge count')
  if type(nodes)=='number' then assert(math.tointeger(nodes) and nodes>=0,'adjacency: invalid node count') end
  local out={}
  if type(nodes)=='number' then for i=0,nodes-1 do out[i]={} end
  else for _,key in ipairs(nodes) do assert(not out[key],'adjacency: duplicate key');out[key]={} end end
  for i=0,edge_count-1 do
    local a,b=endpoints(i)
    assert(out[a] and out[b],'adjacency: unknown endpoint')
    out[a][#out[a]+1]=b;out[b][#out[b]+1]=a
  end
  return out
end

-- FIFO с явной политикой принятия состояния. Transition вызывается для КАЖДОЙ дуги,
-- даже посещённой: consumer может учитывать рёбра отдельно от открытия вершины.
-- seed доступен и внутри transition (динамические источники); повторный seed разрешён
-- только явно, поэтому бюджет ограничивает и ошибочную циклическую политику.
function A.propagate(args)
  local state=args.state or {}
  local pending,head={},1
  local limit=assert(math.tointeger(args.max_visits),'propagate: max_visits is required')
  assert(limit>=0,'propagate: negative max_visits')
  local function seed(key,value)
    assert(args.adjacency[key],'propagate: unknown node')
    assert(value~=nil,'propagate: nil state')
    assert(#pending<limit,'propagate: visit budget exceeded')
    state[key]=value;pending[#pending+1]=key
  end
  args.initial(seed,state)
  while head<=#pending do
    local current=pending[head];head=head+1
    for _,next in ipairs(args.adjacency[current]) do
      local value=args.transition(current,next,state[current],state[next],seed)
      if value~=nil then seed(next,value) end
    end
  end
  return state,#pending
end

function A.nearest_pair(sites,point,options)
  options=options or {}
  local coordinates=options.axes or axes
  local keys=options.keys or {}
  local first,second,d1,d2=nil,nil,math.huge,math.huge
  local function before(d,site,old_d,old)
    if d~=old_d then return d<old_d end
    if not old then return true end
    for _,key in ipairs(keys) do
      if site[key]~=old[key] then return site[key]<old[key] end
    end
    return false -- полный tie сохраняет входной порядок
  end
  for _,site in ipairs(sites) do
    local d=0
    for _,axis in ipairs(coordinates) do d=d+(point[axis]-site[axis])^2 end
    assert(d==d and d<math.huge,'nearest_pair: non-finite distance')
    if before(d,site,d1,first) then second,d2=first,d1;first,d1=site,d
    elseif before(d,site,d2,second) then second,d2=site,d end
  end
  assert(second,'nearest_pair: at least two sites required')
  return first,second,d1,d2
end

function A.nearest_order(count,point,start)
  assert(math.tointeger(count) and count>=0,'nearest_order: invalid count')
  if count==0 then return {} end
  start=start or 0
  assert(start>=0 and start<count,'nearest_order: invalid start')
  local visited,order={[start]=true},{start}
  for _=2,count do
    local p=point(order[#order]);local best,distance=nil,nil
    for candidate=0,count-1 do if not visited[candidate] then
      local q=point(candidate)
      local dx,dy,dz=q.x-p.x,q.y-p.y,q.z-p.z
      local d=dx*dx+dy*dy+dz*dz
      assert(d==d and d<math.huge,'nearest_order: non-finite distance')
      if distance==nil or d<distance then best,distance=candidate,d end
    end end
    visited[best]=true;order[#order+1]=best
  end
  return order
end

-- Две схемы одной кривой сохраняют порядок float операций прежних consumers.
-- Это часть reproducibility, а не два разных семейства кривых.
function A.catmull_rom(a,b,c,d,t,scheme)
  local out={}
  if scheme=='weights' then
    local t2=t*t;local t3=t2*t
    local a0=-0.5*t3+t2-0.5*t
    local a1=1.5*t3-2.5*t2+1.0
    local a2=-1.5*t3+2.0*t2+0.5*t
    local a3=0.5*t3-0.5*t2
    for _,axis in ipairs(axes) do out[axis]=a0*a[axis]+a1*b[axis]+a2*c[axis]+a3*d[axis] end
  else
    assert(scheme==nil or scheme=='polynomial','catmull_rom: unknown evaluation scheme')
    for _,axis in ipairs(axes) do
      out[axis]=0.5*((2*b[axis])+(-a[axis]+c[axis])*t+(2*a[axis]-5*b[axis]+4*c[axis]-d[axis])*t*t+(-a[axis]+3*b[axis]-3*c[axis]+d[axis])*t*t*t)
    end
  end
  return out
end

function A.frame(delta,options)
  options=options or {}
  local length=math.sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z)
  assert(length>0 and length<math.huge,'frame: invalid direction')
  local inverse=1/length
  local tangent={x=delta.x*inverse,y=delta.y*inverse,z=delta.z*inverse}
  local d=options.raw_direction and delta or tangent
  local horizontal=math.sqrt(d.x*d.x+d.z*d.z)
  local right=horizontal<0.001 and {x=1,y=0,z=0} or {x=-d.z/horizontal,y=0,z=d.x/horizontal}
  local up={x=-d.y*right.z,y=d.x*right.z-d.z*right.x,z=d.y*right.x}
  if options.normalize_up then
    local k=1/math.sqrt(up.x*up.x+up.y*up.y+up.z*up.z)
    up={x=up.x*k,y=up.y*k,z=up.z*k}
  end
  if options.positive_y and up.y<0 then up={x=-up.x,y=-up.y,z=-up.z} end
  return right,up,tangent,length
end

-- Связывание полей один раз на шаг. Радиусы, роли, influence/detail bounds и теги
-- остаются авторскими данными; writer не выдумывает для них общую гарантию.
function A.segment_writer(buffer,options)
  local fields={}
  for _,name in ipairs({'a','b','right','up','length2'}) do fields[name]=buffer:field(name) end
  return function(index,a,b)
    assert(index>=0 and index<buffer:count(),'segment_writer: capacity exceeded')
    local right,up,tangent,length=A.frame({x=b.x-a.x,y=b.y-a.y,z=b.z-a.z},options)
    for name,value in pairs({a=a,b=b,right=right,up=up}) do
      for component,axis in ipairs(axes) do fields[name]:set(index,value[axis],component-1) end
    end
    fields.length2:set(index,length*length)
    return right,up,tangent,length
  end
end

function A.tree_distance(tree,a,b)
  local up={};local node,d=a,0.0
  while node do
    assert(tree[node] and up[node]==nil,'tree_distance: invalid parent chain')
    up[node]=d;d=d+(tree[node].span or 0.0);node=tree[node].parent
  end
  node,d=b,0.0;local seen={}
  while node and up[node]==nil do
    assert(tree[node] and not seen[node],'tree_distance: invalid parent chain')
    seen[node]=true;d=d+(tree[node].span or 0.0);node=tree[node].parent
  end
  assert(node,'tree_distance: disconnected trees')
  return d+up[node]
end
return A
